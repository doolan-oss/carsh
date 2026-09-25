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

# 2. Fetch + build circle-stdlib (Circle + newlib + libstdc++, STDLIB_SUPPORT=4).
#    Needs `texinfo` installed (newlib's libgloss builds docs).
../scripts/fetch_circle.sh
cd ../third_party/circle-stdlib
./configure -r 4 --prefix aarch64-none-elf-
make -j$(nproc)

# 3. USB-C serial-log gadget: set a non-zero USB Vendor ID (the default 0x0000 is
#    rejected by Circle's gadget driver). carsh uses the pid.codes open-source VID
#    0x1209 for development — replace before shipping a product. Edit
#    libs/circle/include/circle/sysconfig.h:
#        #define USB_GADGET_VENDOR_ID  0x1209
#    then rebuild just the gadget lib:
cd libs/circle/lib/usb/gadget && make
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
> Plain Circle is `-nostdinc`; full STL needs `STDLIB_SUPPORT=4`, which
> circle-stdlib provides (newlib + the toolchain's libstdc++).

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
run.

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
qemu-system-aarch64 -M raspi3b -kernel kernel8-rpi3.img -serial stdio -display none
# then reconfigure back to -r 4 and rebuild for the product image
```

## Bluetooth (A2DP sink + AVRCP)

The app binds to `bt::IBluetooth`; `device/CircleBluetooth` implements it. All
BTstack-specific code is in `device/BtStackGlue.cpp`, **gated behind
`CARSH_HAVE_BTSTACK`** — the default build links a stub (no BT), so the rest of
the image is unaffected. The kernel already wires the seam: connect → driver
profile switch, AVRCP metadata → BT now-playing, decoded A2DP PCM → `g_btRing`,
and a `CircleBtRunLoopTask` that pumps the BTstack run loop.

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
- `HalCircle.cpp` — the embedded HAL (`hal_time_ms` via `CTimer`, `hal_cpu_*`
  via Circle critical sections).
- `CircleHciTransport.cpp` — a `hci_transport_t` backed by Circle's
  `CUSBBluetoothDevice` (the stage-1 USB-dongle path).
- `BtStackClassicShims.c` — the `hci_le_rand` command descriptor (BLE-gated
  upstream) so `btstack_crypto` links in a Classic-only config.

`BtStackGlue.cpp` holds the real A2DP-sink + AVRCP + GAP glue (modelled on
`example/a2dp_sink_demo.c`), and `src/bt/NowPlaying.h` assembles the per-attribute
AVRCP now-playing subevents into one `bt::AvrcpMetadata` (host-tested in
`tests/bt/BtParse_test`).

### Two-stage hardware path

Mirrors the audio bring-up: get A2DP working first over a **USB BT dongle**
(Realtek RTL8761B, e.g. TP-Link UB500) via `CUSBBluetoothDevice` → BTstack
`hci_transport` (simpler, no onboard firmware), then port to the onboard
CYW43455. The build above targets stage 1.

### What still needs the bench (TODO(hw), see HARDWARE_TODO H6)

The build links, but it is **not** validated against a radio:

1. **USB ACL data path — the one real transport gap.** Circle's
   `CUSBBluetoothDevice` configures the bulk IN/OUT endpoints but exposes no ACL
   transfer API (only the control EP for commands + the interrupt EP for events).
   `CircleHciTransport.cpp` therefore wires HCI **command/event** (so HCI
   bring-up, inquiry and connection can work) but **drops ACL** — no A2DP audio
   flows until `CUSBBluetoothDevice` is extended with bulk ACL in/out. Note
   `third_party/circle-stdlib/` is git-ignored, so that extension must ship as a
   tracked patch under `scripts/`, not an in-place edit.
2. **Realtek firmware blobs.** `BtStackGlue` points the chipset at
   `SD:/firmware/rtlbt`; the matching `rtlbt_fw` / `rtlbt_config` for the dongle's
   USB product id must be placed there (R3).
3. **IRQ-context delivery.** Circle delivers HCI events from USB-completion
   context; for correctness they should be trampolined onto the BTstack run loop
   rather than handed to the stack synchronously.
4. **SSP / pairing** and the **clock-drift adaptive SRC** (R12) on the A2DP PCM.

## Current milestone — booting on real hardware

The image **boots on a real Pi 4 and runs as a media player.** Validated on the
board: framebuffer/DSI panel, touch input, USB FAT32 media indexed into the
library, onboard PWM audio out the 3.5 mm jack, the vertical volume rail, and the
USB-C serial console. The decode + scheduler tasks run live feeding playback.

Remaining hardware work (see ../HARDWARE_TODO.md): I2S DAC HAT (PWM jack works
today), BTstack on a real radio (USB ACL path + Realtek firmware blob — H6),
WiFi, and the OTA flash write + chainloader. Backends that build but await their
peripheral: `CircleSoundDevice` (I2S), `CircleFlashTarget` (OTA slot write).

## Serial OTA dev loop (no card swap)

Once per card: install the A/B chainloader (`device/chainloader/README.md`).
Once per bench: `scripts/carsh_ota.py keygen` (writes `device/OtaPushKey.h`),
then rebuild the image so it trusts your key. After that the loop is:

```sh
cd device && make                    # build the app image
../scripts/carsh_ota.py flash        # sign + push over /dev/ttyACM0, reboot, watch
```

The push travels the same USB-C console the log rides on (PushProtocol framing,
CRC-32 per packet + whole-blob, Ed25519-signed .carup). The kernel stages it to
the inactive slot (`kernel8-A/B.img`), arms a 3-boot trial in `boot_slot.bin`,
and reboots; the chainloader rolls back automatically if the new image never
reaches the UI loop. Bench validation checklist: HARDWARE_TODO.md H7b.
