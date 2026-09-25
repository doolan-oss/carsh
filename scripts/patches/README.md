# circle-stdlib patches

`third_party/circle-stdlib/` is git-ignored (it's a large vendored tree), so local
modifications to it live here as tracked patches and must be re-applied after a
fresh `scripts/fetch_circle.sh`.

On a fresh checkout, apply all seven before the first circle-stdlib build (they apply
cleanly in this order to the pinned Circle Step51), then the gadget VID edit below:
```sh
for p in usbbluetooth-acl usbdevicefactory-aoa usbstandardhub-reconfigure \
         sysconfig-multicore sysconfig-heap-buckets chainboot-multicore \
         usbcdcgadget-rx-overrun; do
  patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/$p.patch
done
```
On an already-built circle-stdlib, rebuild per patch as noted in each section. The
two `sysconfig-*` patches change `sysconfig.h`, which is compiled into most of
Circle, so they need a **full libcircle rebuild** (`./makeall clean && ./makeall`),
not one directory.

**Not a patch — the USB gadget vendor ID.** Circle's default
`USB_GADGET_VENDOR_ID` is 0x0000, which its gadget driver rejects, so without this
edit there is no USB-C serial console (`/dev/ttyACM0`), no bench log and no serial OTA
push. It is a one-line manual edit of `libs/circle/include/circle/sysconfig.h`
(`#define USB_GADGET_VENDOR_ID 0x1209`, the pid.codes development VID), described in
`device/README.md` step 3; rebuild `lib/usb/gadget` afterwards (or apply it before the
first build).

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
bind its bulk endpoints. Bench-confirmed root cause and fix (AA_PLAN rung 3: the
accessory device binds its bulk endpoints as `uaa1`).

Apply + rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/usbstandardhub-reconfigure.patch
( cd third_party/circle-stdlib/libs/circle/lib/usb && make )
```

## sysconfig-multicore.patch
Enables `ARM_ALLOW_MULTI_CORE` in Circle's `sysconfig.h`. Needed so the Android Auto
H.264 decode can run on a **dedicated A72 core** (`device/CircleAaDecodeCore.h`,
`CMultiCoreSupport`) instead of starving the core-0 audio refill — the bench-measured
cause of choppy AA audio (decode ~37 ms/frame > the 33 ms real-time budget). Enabling
it also maps RAM **inner-shareable cacheable**, which makes the lock-free cross-core
handoffs (`src/aa/AuQueue.h` AU input, `src/aa/FrameRelay.h` decoded-frame output)
coherent without explicit cache maintenance. The whole `libcircle` must be rebuilt
(the define changes the MMU setup + secondary-core spin-up), not just one dir.

Apply + rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/sysconfig-multicore.patch
( cd third_party/circle-stdlib/libs/circle && ./makeall clean && ./makeall -j$(nproc) )
```
The device image gets the dedicated core by default with `make CARSH_HAVE_OPENH264=1`;
build `CARSH_NO_DECODE_CORE=1` to fall back to the single-core cooperative decode task.

## sysconfig-heap-buckets.patch
Adds large heap buckets to Circle's `HEAP_BLOCK_BUCKET_SIZES` in `sysconfig.h`:
0xC0000 up to 0x8000000 (768 KiB to 128 MiB), 20 buckets in total, which is Circle's
`HEAP_BLOCK_MAX_BUCKETS` limit. `CHeapAllocator` only puts a freed block back on a free
list when its size equals a bucket. With the stock list, whose largest bucket is
0x80000, every freed malloc/new block over 512 KiB was lost for good; cover-art
decodes, tag buffers, vector regrowth and per-session buffers slowly used up HEAP_LOW
this way (review L1). The hunk is independent of `sysconfig-multicore.patch` and
applies before or after it. Like that patch, it needs a full libcircle rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/sysconfig-heap-buckets.patch
( cd third_party/circle-stdlib/libs/circle && ./makeall clean && ./makeall -j$(nproc) )
```

## chainboot-multicore.patch
Lets Circle's `EnableChainBoot()` work in an `ARM_ALLOW_MULTI_CORE` build. Upstream
`assert (0)`s in every multi-core build, so the A/B chainloader (`device/chainloader/`,
linked against the same libcircle) halted on every boot (review O1). The patch refuses
only once a `CMultiCoreSupport` exists, i.e. once secondary cores may be running the
image that is about to be overwritten. The chainloader never starts them: they stay
parked in the armstub spin table, and the chained app releases them as on a cold boot.
It adds `IsChainBootAllowed()`, `CMultiCoreSupport::IsActive()` and the
`CHAINBOOT_MULTICORE_SAFE` marker. Without the patch the chainloader `#error`s, and a
patched header over a stale `libcircle.a` fails to link.

Apply + rebuild (after `sysconfig-multicore.patch`; it touches `multicore.h`, so
rebuild `lib/` — a full `./makeall clean && ./makeall` for the sysconfig patches
covers it too):
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/chainboot-multicore.patch
( cd third_party/circle-stdlib/libs/circle/lib && make -j$(nproc) )
```

## usbcdcgadget-rx-overrun.patch
Stock Circle asserts inside the USB IRQ when the CDC gadget's 8 KiB RX ring is exactly
full, or when a zero-length packet arrives: `Enqueue()` of 0 bytes trips
`assert (nCount > 0)`. That halts the whole board instead of reporting the overrun
(-1) to `Read()`. With this patch, `CircleUsbLog` logs the overrun and fails an
in-progress OTA push with `rx-overrun`, and the host restarts it. The serial push is
flow-controlled, so the ring should never fill; this is defence in depth (review O2).

Apply + rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/usbcdcgadget-rx-overrun.patch
( cd third_party/circle-stdlib/libs/circle/lib/usb/gadget && make )
```
