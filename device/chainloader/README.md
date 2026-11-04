# carsh A/B chainloader

The tiny boot kernel behind the serial-OTA dev loop (and, later, end-user OTA).
Every boot: read `SD:/boot_slot.bin` → run the host-tested try/rollback decision
(`src/ota/BootSlots.h`) → persist the consumed try → load `SD:/kernel8-A.img`
or `-B.img` → warm-jump to it (Circle chainboot). A pushed image that never
comes up healthy exhausts its 3 tries and the chainloader rolls back to the
last-known-good slot — the push loop cannot brick the card.

## Build

carsh's libcircle is multi-core (`scripts/patches/sysconfig-multicore.patch`, for
the AA decode core), and stock Circle refuses chain boot in any multi-core build
(`EnableChainBoot()` is `assert (0)`). So circle-stdlib must also carry
`scripts/patches/chainboot-multicore.patch`, which allows chain boot as long as no
secondary core was ever started. That holds here: the chainloader never creates a
`CMultiCoreSupport`, so cores 1-3 stay parked in the armstub spin table and the
chained app releases them as it would on a cold boot. Without the patch,
`kernel.cpp` stops the build with an `#error`. A patched header over a stale
`libcircle.a` fails to link (`IsChainBootAllowed`).

```sh
export PATH=/opt/arm-toolchain/bin:$PATH
# once per circle-stdlib checkout (after sysconfig-multicore.patch):
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/chainboot-multicore.patch
( cd third_party/circle-stdlib/libs/circle/lib && make -j$(nproc) )
cd device/chainloader && make       # -> kernel8-rpi4.img (the chainloader)
```

## One-time card migration (bench)

The chainloader replaces the app image as the card's boot kernel; the app moves
to slot A:

```sh
# card mounted at $CARD (see the bench loop in CLAUDE.md)
cp device/kernel8-rpi4.img             $CARD/kernel8-A.img   # app -> slot A
cp device/chainloader/kernel8-rpi4.img $CARD/kernel8-rpi4.img # chainloader boots
sync
```

No `boot_slot.bin` is needed: the chainloader bootstraps one (slot A, healthy)
on first boot, and the kernel's push service does the same if a push arrives
before the migration. From then on the SD card never leaves the Pi:

```sh
scripts/carsh_ota.py keygen                       # once; then rebuild the image
cd device && make                                 # build the app
scripts/carsh_ota.py flash --log bench.log        # push over /dev/ttyACM0 + watch
```

The push is flow-controlled. The kernel logs `ota-push: ack <seq>` as it drains
the USB-C gadget's 8 KiB receive ring, and the script keeps at most 6 KiB
un-acked (`src/ota/PushProtocol.h`). The acks ride the console, so stop
`scripts/bench_capture.py` for the push: the script refuses to start while
another process has the port open. `--log FILE` keeps a raw capture in the
meantime. A failed receive restarts the transfer from BEGIN (`--retries`,
default 2).

## Boot sequence after migration

```
Pi firmware -> kernel8-rpi4.img (chainloader, GPIO-UART log only)
            -> decide(boot_slot.bin): steady / trial (tries--) / rollback
            -> chainboot SD:/kernel8-{A,B}.img (the carsh app)
```

`[hw]` to validate on the bench (HARDWARE_TODO): chainboot jump on the Pi 4,
trial + confirm on a good push, forced rollback (push an image that hangs
before `confirmBootHealthy`, power-cycle 3×, expect slot fallback).
