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

Copy `kernel8-rpi4.img` (renamed `kernel8.img`) + the Pi firmware + `config.txt`
to a FAT32 SD card. With `enable_uart=1`, the boot log appears on the PL011 UART
(GPIO14/15) at 115200 — or route it to the HDMI console.

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

## Current milestone (Step 2)

`CKernel` brings up serial logging + the framebuffer and renders the real carsh
UI (the same `ui/` toolkit the host tests cover) to the panel. Next milestones
wire the remaining backends: FAT (`Vfs`), I2S DAC (`ISoundDevice`), touch
(`ITouch`), the decode/scheduler tasks, then BTstack and OTA flash.
