# carsh device build (Circle / bare metal)

Builds the `kernel8-rpi4.img` that boots on the Pi 4. The platform-independent
core (`../src`) — proven by the host tests — links unchanged against the Circle
device backends here.

## One-time setup

```sh
# 1. Bare-metal AArch64 toolchain (Arm GNU, newlib-based). Put it on PATH:
#    https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
#    (carsh is built/tested with 14.2.Rel1 aarch64-none-elf)
export PATH=/opt/arm-toolchain/bin:$PATH

# 2. Fetch circle-stdlib (Circle + newlib + libstdc++).
../scripts/fetch_circle.sh
cd ../third_party/circle-stdlib

# 3. Before the first build: apply carsh's seven tracked Circle patches (see
#    ../scripts/patches/README.md — without them no image builds (kernel.cpp
#    checks for the heap-bucket patch), the AA image doesn't compile, the dongle
#    never binds and the chainloader doesn't build), then set a
#    non-zero USB Vendor ID for the USB-C serial-log gadget (the default 0x0000 is
#    rejected by Circle's gadget driver; this one is a manual edit, not a patch).
#    carsh uses the pid.codes open-source VID 0x1209 for development — replace
#    before shipping a product. Edit libs/circle/include/circle/sysconfig.h:
#        #define USB_GADGET_VENDOR_ID  0x1209
for p in usbbluetooth-acl usbdevicefactory-aoa usbstandardhub-reconfigure \
         sysconfig-multicore sysconfig-heap-buckets chainboot-multicore \
         usbcdcgadget-rx-overrun; do
  patch -p1 -d libs/circle < ../../scripts/patches/$p.patch
done

# 4. Build circle-stdlib. `./configure -r 4` targets the Pi 4 with STDLIB_SUPPORT=3
#    (newlib + the toolchain's libstdc++). Needs `texinfo` installed (newlib's
#    libgloss builds docs). On an already-built tree, a patch or VID change needs
#    the rebuild noted in scripts/patches/README.md (all of libcircle for the two
#    sysconfig patches; just lib/usb/gadget for the VID).
./configure -r 4 --prefix aarch64-none-elf-
make -j$(nproc)
```

### USB-C serial console (no extra cable)

The image presents a USB CDC-ACM **serial gadget** on the Pi 4's USB-C port (the
dwc2 OTG controller, independent of the xHCI host that drives the USB-A ports).
With the Pi bus-powered from a host PC over USB-C, the firmware log streams to
that PC as `/dev/ttyACM0` — the logger retargets to it on connect, and falls back
to the PL011 UART (GPIO14/15) otherwise. Read it with e.g.
`picocom -b 115200 /dev/ttyACM0` (the CDC baud is nominal). This coexists with the
USB-A media stick and the onboard Bluetooth.

> Why circle-stdlib and not plain Circle? carsh's core uses the C++ STL
> (`std::vector`/`string`/`map`/`function`, `<cmath>`, `<atomic>`, `<complex>`).
> Plain Circle is `-nostdinc`; full STL needs circle-stdlib's `STDLIB_SUPPORT=3`
> (newlib + the toolchain's libstdc++; `4` would switch to LLVM libc++).

## Build the image

```sh
cd device
export PATH=/opt/arm-toolchain/bin:$PATH
# One-time: build the bundled mbedTLS for the Android Auto TLS channel (the default
# image now links AaCryptoMbedTls). See AA_PLAN rung 4b.
( cd ../third_party/circle-stdlib && make mbedtls )
make                # -> kernel8-rpi4.img
```

The default image links the **AA TLS crypto** (mbedTLS, rung 4b). To also link the
**H.264 decoder** for live video projection, vendor + cross-build openh264
(`scripts/fetch_openh264.sh`) and build with `make CARSH_HAVE_OPENH264=1` (rung 5);
without it the AA tab shows its placeholder but TLS / channels / audio / touch still
run. The bench image is `make CARSH_HAVE_OPENH264=1 CARSH_HAVE_BTSTACK=1`. Run
`make clean` whenever you switch between `CARSH_*` variants: the Makefile doesn't
track the flags, so objects built for another variant are silently reused.

Copy `kernel8-rpi4.img` (keep this name — `config.txt`'s `[pi4]` section loads
`kernel=kernel8-rpi4.img`) + the Pi firmware + `config.txt` to a **FAT32** SD card
(the Pi 4 bootloader cannot read other filesystems). With `enable_uart=1`, the
boot log appears on the PL011 UART (GPIO14/15) at 115200 — or route it to the
HDMI console.

The `scripts/make_sdcard.fish` helper assembles a correct boot partition (FAT32 +
the firmware, kernel, and a Pi 4 `config.txt`) from this tree in one step:

```sh
scripts/make_sdcard.fish /dev/sdX   # ERASES the target device
```

## QEMU smoke (runtime proof, no board)

Circle's QEMU support targets **raspi3b** in AArch64. Build a separate QEMU
variant (different Pi model + semihosting):

```sh
cd ../third_party/circle-stdlib
./configure -r 3 --qemu --prefix aarch64-none-elf- && make -j$(nproc)
cd ../../device && make clean && make
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none
# then reconfigure back to -r 4 and rebuild for the product image
```

## Bluetooth (A2DP sink + AVRCP)

The app binds to `bt::IBluetooth`; `device/CircleBluetooth` implements it. All
BTstack-specific code is in `device/BtStackGlue.cpp`, **gated behind
`CARSH_HAVE_BTSTACK`** — the default build links a stub (no BT), so the rest of
the image is unaffected. The kernel already wires the seam: connect → driver
profile switch, AVRCP metadata → BT now-playing (and `onAvrcpEnded` → back to A2DP
activity when the phone's AVRCP session ends), decoded A2DP PCM → `g_btRing`, and a
`CircleBtRunLoopTask` that pumps the BTstack run loop.

### Building it

```sh
../scripts/fetch_btstack.sh        # pin BTstack under third_party/btstack (once)
make CARSH_HAVE_BTSTACK=1           # compiles + links BTstack into the image
```

This **compiles and links today** (image grows ~1.12 MB → ~1.37 MB). The build
pulls a curated, Classic-only source closure — core BTstack + the A2DP-sink /
AVRCP / SDP profiles + the bluedroid SBC codec + the embedded run loop — plus
three carsh files:

- `btstack_config.h` — Classic-only A2DP-sink + AVRCP, embedded run loop, no
  BLE/mesh (small footprint).
- `HalCircle.cpp` — the embedded HAL (`hal_time_ms` from carsh's counter clock,
  `device/CircleClock.h`, `hal_cpu_*` via Circle critical sections).
- `CircleHciTransport.cpp` — a `hci_transport_t` backed by Circle's
  `CUSBBluetoothDevice` (the stage-1 USB-dongle path).
- `BtStackClassicShims.c` — the `hci_le_rand` command descriptor (BLE-gated
  upstream) so `btstack_crypto` links in a Classic-only config.

`BtStackGlue.cpp` holds the real A2DP-sink + AVRCP + GAP glue (modelled on
`example/a2dp_sink_demo.c`), and `src/bt/NowPlaying.h` assembles the per-attribute
AVRCP now-playing subevents into one `bt::AvrcpMetadata` (host-tested in
`tests/bt/BtParse_test`). It registers all three AVRCP packet handlers (avrcp /
avrcp_controller / avrcp_target) and the A2DP sink, AVRCP CT, AVRCP TG and Device ID
SDP records (four records = `MAX_NR_SERVICE_RECORD_ITEMS` in `btstack_config.h`), and
feeds HCI/A2DP/AVRCP connection events into `src/bt/LinkTracker.h` (host-tested in
`tests/bt/LinkTracker_test`), which decides connected, disconnected and
connect-failed. The AVRCP TG implements absolute volume: the phone's
SetAbsoluteVolume is applied as a gain on the A2DP PCM (`src/bt/AbsoluteVolume.h`),
ahead of the HU master rail.

### Two-stage hardware path

Mirrors the audio bring-up: get A2DP working first over a **USB BT dongle**
(Realtek RTL8761B, e.g. TP-Link UB500) via `CUSBBluetoothDevice` → BTstack
`hci_transport` (simpler, no onboard firmware), then port to the onboard
CYW43455. The build above targets stage 1.

### What still needs the bench

A2DP audio works on the board (UB500 dongle, `scripts/patches/usbbluetooth-acl.patch`
for the ACL path, Realtek firmware from `SD:/firmware/rtlbt`). AVRCP metadata /
transport / absolute volume and the connect/disconnect state are fixed in code but
not yet benched — see HARDWARE_TODO H6 and H9 §F. Still open: bonding (no link-key
DB), SSP pairing UX and the clock-drift adaptive SRC (R12) on the A2DP PCM.

## Current milestone — booting on real hardware

The image **boots on a real Pi 4 and runs as a media player.** Validated on the
board: framebuffer/DSI panel, touch input, USB FAT32 media indexed into the
library, onboard PWM audio out the 3.5 mm jack, the vertical volume rail, and the
USB-C serial console. The decode + scheduler tasks run live feeding playback.

Remaining hardware work (see ../HARDWARE_TODO.md): I2S DAC HAT (PWM jack works
today), Bluetooth AVRCP (A2DP audio already plays — H6), WiFi, and the serial OTA
push + A/B chainloader (H7b). Everything fixed on 2026-09-25 is bench-pending (H9).
Backends that build but await their peripheral: `CircleSoundDevice` (I2S),
`CircleFlashTarget` (OTA slot write).

## Serial OTA dev loop (no card swap)

Once per card: install the A/B chainloader (`device/chainloader/README.md`) with an
app image from 2026-09-25 or later — older kernels don't ack the flow-controlled
push, so this migration is the last card swap. Once per bench:
`scripts/carsh_ota.py keygen` (writes `device/OtaPushKey.h`), then rebuild the image
so it trusts your key. After that the loop is:

```sh
cd device && make                                # build the app image
../scripts/carsh_ota.py flash --log ../bench.log # sign + push over /dev/ttyACM0, reboot, watch
```

The push travels the same USB-C console the log rides on (PushProtocol framing,
CRC-32 per packet + whole-blob, Ed25519-signed .carup). The transfer is
flow-controlled: the kernel acks each packet it has drained from the gadget's 8 KiB
receive ring (`ota-push: ack <seq>`) and the host keeps at most 6 KiB un-acked,
because Circle's CDC gadget drops bytes that don't fit instead of pushing back. Stop
`scripts/bench_capture.py` during a push (the script refuses otherwise); a failed
receive restarts from BEGIN (`--retries`). The chainloader needs
`scripts/patches/chainboot-multicore.patch`. The kernel stages the image to
the inactive slot (`kernel8-A/B.img`), arms a 3-boot trial in `boot_slot.bin`,
and reboots; the chainloader rolls back automatically if the new image never
reaches the UI loop. Bench validation checklist: HARDWARE_TODO.md H7b + H9 §G.
