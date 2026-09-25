# CLAUDE.md — agent onboarding for carsh

carsh is a **bare-metal automotive head unit firmware for the Raspberry Pi 4**,
built on the **Circle** framework (C++17, no OS). It boots on a real Pi 4 as a media
player (DSI panel + touch + USB media + PWM/Bluetooth audio + USB-C serial console)
and is growing wireless Android Auto support.

## Hard constraints — read first

- **No Linux. Ever.** carsh is bare-metal Circle on the Pi 4 and stays that way. Do
  not propose Linux, a Linux-based distro, or "just run X on Linux" — it's a settled
  decision, not an open question. The whole value is a from-cold-boot appliance.
- **Interface-first, host-tested.** Every subsystem is written against a HAL interface
  (`IRenderTarget`, `ISoundDevice`, `Vfs`, `ITouch`, `IAaTransport`, …) with a host
  backend so it's unit-tested on the build server with **no hardware**. "host-done" =
  compiles + host tests pass; it never means "validated on the Pi" (see `STATUS.md`).
- **Don't commit/push unless asked.** Branch off `main` for changes. circle-stdlib is
  git-ignored (see below) — never commit into it.

## Where things live

| Path | What |
|---|---|
| `src/` | platform-independent core (UI, audio, library, decode, OTA, `src/aa/` = AA protocol) |
| `host/` | host backends (WAV sink, PNG target, host FS, fakes) used by tests |
| `tests/` | host unit tests (CMake globs `tests/**/*.cpp`) |
| `device/` | Circle backends + `kernel.cpp` (the device image entry) |
| `third_party/circle-stdlib/` | **git-ignored** vendored Circle + newlib + mbedTLS |
| `scripts/patches/` | tracked patches to circle-stdlib (it's git-ignored) + how to apply |
| `STATUS.md` | per-module state (hw-validated / host-done / stubbed) — **the source of truth** |
| `AA_PLAN.md` | Android Auto plan + the live bench-bring-up log + next steps |
| `HARDWARE_TODO.md` | bench backlog (things only the board can validate) |
| `README.md` / `device/README.md` | host build / device build setup |

## Build & test

**Host (fast, no hardware) — do this for any `src/`/`tests/` change:**
```sh
cmake -S . -B build && cmake --build build -j
./build/carsh_tests                 # all tests; pass a substring to filter
```

**Device image (cross-compiled, aarch64-none-elf bare metal):**
```sh
export PATH=/opt/arm-toolchain/bin:$PATH           # the cross toolchain
# one-time: scripts/fetch_circle.sh then build circle-stdlib (STDLIB_SUPPORT=4)
cd device && make                                   # -> device/kernel8-rpi4.img
cd device && make CARSH_HAVE_BTSTACK=1              # variant with BTstack (A2DP) linked
```
Default `make` links a **stub** for Bluetooth; the rest of the image is real.

**mbedTLS (for the AA TLS work):** `cd third_party/circle-stdlib && make mbedtls`
builds `libmbed{crypto,x509,tls}.a` + `libcircle-mbedtls.a` (bare-metal config +
entropy via Circle's hardware RNG).

## circle-stdlib patches (important)

`third_party/circle-stdlib/` is git-ignored, so local edits to it are tracked as
patches under `scripts/patches/` and **must be re-applied after a fresh checkout**
(see `scripts/patches/README.md`). Current patches: a USB Bluetooth ACL path, an
Android-Auto USB factory hook, and a USB hub reconfigure fix. After patching, rebuild
the affected lib (e.g. `cd third_party/circle-stdlib/libs/circle/lib/usb && make`)
then relink the image.

## The bench loop (hardware iteration)

Device work is validated by flashing an SD card and reading the **USB-C serial
console** (a CDC-ACM gadget on the dwc2 port → `/dev/ttyACM0` on the dev host). The
loop, with the card in a USB reader as `/dev/sda1` (label `CARSHBOOT`):
```sh
udisksctl mount -b /dev/sda1
cp device/kernel8-rpi4.img /run/media/$USER/CARSHBOOT/ && sync
udisksctl unmount -b /dev/sda1
# move card to the Pi, power on; capture the console (run in background, it survives
# power-cycles): a loop that `cat`s /dev/ttyACM0 (115200 raw) to a log file.
```
**No-card-swap alternative (once the card is migrated to A/B — see
`device/chainloader/README.md`):** `scripts/carsh_ota.py flash` pushes the freshly
built image over the same USB-C console; the kernel verifies + stages it to the
inactive slot and reboots through the chainloader (bad image = automatic rollback,
3 tries). One-time setup: `carsh_ota.py keygen` then rebuild. `[hw]`-pending: H7b.

Bare metal has no remote shell — **route diagnostics to the serial log and iterate**.
The USB-C console is brought up before the USB host in `kernel.cpp` so enumeration
logs are captured. Batch your logging up-front; each flash is a manual card swap.

## Current focus: wireless Android Auto via a USB dongle

Approach: a commercial wireless-AA dongle (the owner's is a **MINIX CA525**) plugs
into a Pi 4 USB-A port; it owns the wireless (BT + WiFi to the phone) and presents as
a **wired-AA phone over USB**. carsh is the **USB-host wired-AA head unit**. The AA
protocol core (`src/aa/`: framing, protobuf, connection state machine, H.264 NAL
parse) is host-tested and merged. Bring-up status (full log in `AA_PLAN.md`):

- **Done on hardware (rungs 1–4a):** dongle enumerates (`ven2717-ff40`, Android PTP)
  → AOA mode-switch → re-enumerates in accessory mode (`ven18d1-2d00`) → bulk
  endpoints bound + registered as `uaa1` (`device/CircleAoaTransport.cpp` =
  `CAndroidAutoDevice`, a `CUSBFunction` + `IAaTransport`) → `CircleAaDriver` drives
  `AaConnection` over it and the **plaintext VERSION_REQUEST/RESPONSE exchange with
  the phone succeeds** (bidirectional bulk proven). A logging stub crypto stops at TLS.
- **Next (rung 4b — TLS):** fully scoped + de-risked in `AA_PLAN.md`. mbedTLS builds
  for Circle; port `dylant9/head_unit:main/aa_crypto.c` → `device/AaCryptoMbedTls`
  (implements `IAaCrypto`; HU = SSL client, `VERIFY_NONE`, in-memory BIO, the embedded
  aasdk head-unit cert — note the x509 date-parse bypass gotcha), link the mbedTLS
  `.a`s, swap out `LoggingCrypto`. Then service discovery → channels → H.264 decode
  (rung 5, software/openh264 on a dedicated A72 core) → the `AndroidAuto` UI tab
  (already built: aspect-fit video blit + touch remap in `src/ui/AndroidAutoScreen.h`).

When picking up: read `AA_PLAN.md` (plan + bench log) and `STATUS.md` (module state).
