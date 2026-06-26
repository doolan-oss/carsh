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
```

> Why circle-stdlib and not plain Circle? carsh's core uses the C++ STL
> (`std::vector`/`string`/`map`/`function`, `<cmath>`, `<atomic>`, `<complex>`).
> Plain Circle is `-nostdinc`; full STL needs `STDLIB_SUPPORT=4`, which
> circle-stdlib provides (newlib + the toolchain's libstdc++).

## Build the image

```sh
cd device
export PATH=/opt/arm-toolchain/bin:$PATH
make                # -> kernel8-rpi4.img
```

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

## Bluetooth (A2DP sink + AVRCP) — scaffold

The app binds to `bt::IBluetooth`; `device/CircleBluetooth` implements it. All
BTstack-specific code is in `device/BtStackGlue.cpp`, **gated behind
`CARSH_HAVE_BTSTACK`** — the default build links a stub (no BT), so the rest of
the image is unaffected. The kernel already wires the seam: connect → driver
profile switch, AVRCP metadata → BT now-playing, decoded A2DP PCM → `g_btRing`,
and a `CircleBtRunLoopTask` that pumps the BTstack run loop.

To make it live (at the bench, with a controller):

```sh
../scripts/fetch_btstack.sh           # pin BTstack under third_party/btstack
```

Then, modelled on **BTstack `port/raspi`** + `example/a2dp_sink_demo.c` / `avrcp_*`:
1. Implement the HCI UART transport (`btstack_uart_block_t`) over a Circle
   `CSerialDevice` on the **controller's** PL011 (not the debug console UART).
2. `hci_set_chipset(btstack_chipset_bcm_instance())` and pin the BCM/CYW `.hcd`
   firmware on the SD boot partition (Risk R3).
3. Init `a2dp_sink` + `avrcp` and complete the packet handlers in
   `BtStackGlue.cpp` to call `owner->deliverConnected/Metadata/A2dpPcm(...)`.
4. Add BTstack's `src/` + `classic/{a2dp_sink,avrcp}.c` to the device build and
   compile with `-DCARSH_HAVE_BTSTACK`.

Two-stage path (mirrors the audio bring-up): get A2DP working first over a **USB
BT dongle** via Circle's `CUSBBluetoothDevice` → BTstack `hci_transport`
(simpler, no onboard firmware), then port to the onboard CYW43455.

## Current milestone (Step 2)

`CKernel` brings up serial logging + the framebuffer and renders the real carsh
UI (the same `ui/` toolkit the host tests cover) to the panel. Next milestones
wire the remaining backends: FAT (`Vfs`), I2S DAC (`ISoundDevice`), touch
(`ITouch`), the decode/scheduler tasks, then BTstack and OTA flash.
