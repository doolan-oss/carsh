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
Android-Auto USB factory hook, a USB hub reconfigure fix, and **`sysconfig-multicore`
(enables `ARM_ALLOW_MULTI_CORE`)**. After patching, rebuild the affected lib (e.g.
`cd third_party/circle-stdlib/libs/circle/lib/usb && make`) then relink the image —
**except the multicore patch, which needs a full `libcircle` rebuild** (see the bench
loop above).

## The bench loop (hardware iteration)

Device work is validated by flashing an SD card and reading the **USB-C serial
console** (a CDC-ACM gadget on the dwc2 port → `/dev/ttyACM0` on the dev host). The
loop, with the card in a USB reader as `/dev/sda1` (label `CARSHBOOT`):
```sh
udisksctl mount -b /dev/sda1
cp device/kernel8-rpi4.img /run/media/$USER/CARSHBOOT/ && sync
udisksctl unmount -b /dev/sda1
# move card to the Pi, power on; capture the console (survives power-cycles):
python3 scripts/bench_capture.py bench.log &   # run in background, re-attaches on reboot
```
Bare metal has no remote shell — **route diagnostics to the serial log and iterate**.
The USB-C console is brought up before the USB host in `kernel.cpp` so enumeration
logs are captured. Batch your logging up-front; each flash is a manual card swap.

> **Serial-capture gotcha:** a plain `cat`/`stty` on `/dev/ttyACM0` *blocks on open* —
> the Circle CDC-ACM gadget doesn't assert carrier (DCD), so the open waits forever and
> the capture silently hangs. Use **`scripts/bench_capture.py`** (opens non-blocking
> with `CLOCAL`, re-attaches across power-cycles) — start it once and leave it running.

> **Multi-core (since 2026-06-29):** AA H.264 decode runs on a dedicated A72 core, so
> circle is now built with **`ARM_ALLOW_MULTI_CORE`** (`scripts/patches/sysconfig-multicore.patch`).
> After a fresh `scripts/fetch_circle.sh` you MUST re-apply that patch and **rebuild all
> of libcircle** (`cd third_party/circle-stdlib/libs/circle && ./makeall clean && ./makeall -j`),
> not just one dir — it changes the MMU (inner-shareable cacheable, which makes the
> cross-core SPSC handoffs coherent) + the secondary-core spin-up.

## Current focus: wireless Android Auto via a USB dongle

Approach: a commercial wireless-AA dongle (the owner's is a **MINIX CA525**) plugs
into a Pi 4 USB-A port; it owns the wireless (BT + WiFi to the phone) and presents as
a **wired-AA phone over USB**. carsh is the **USB-host wired-AA head unit**. The AA
protocol core (`src/aa/`: framing, protobuf, connection state machine, H.264 NAL
parse) is host-tested and merged. Bring-up status (full log in `AA_PLAN.md`):

- **Working end-to-end on hardware:** dongle enumerates (`ven2717-ff40` PTP) → AOA
  switch → accessory mode (`ven18d1-2d00`) → bulk endpoints bound as `uaa1` →
  version → **live TLS** (real aasdk cert in `AaHuCert.h`) → service discovery →
  channels → **H.264 video + media audio both play.** AA media is also wired into the
  player UI (Now Playing source badge + transport keys; see `player::MediaSession`).
- **Choppy audio: fixed (2026-06-29, "good enough" on the bench; PR #24 branch
  `claude/aa-decode-core`).** Decode moved to a **dedicated A72 core**
  (`CircleAaDecodeCore` + `ARM_ALLOW_MULTI_CORE`), frames cross to the UI core via a
  lock-free triple buffer (`src/aa/FrameRelay.h`), audio refilled several times per UI
  loop, **video dropped to 480p (800×480, native panel)** so software decode is
  real-time (~15 ms, was ~37 ms at 720p → it had been overflowing the queue, dropping
  keyframes, and freezing the screen), and the `JitterBuffer` got a 480 ms prebuffer +
  quick-resume for the dongle's bursty WiFi audio delivery. Full story in
  `AA_AUDIO_INVESTIGATION.md`.
- **Open / next:** the **intermittent xHCI `Transfer error 4` on the bulk-IN during
  the TLS handshake** — non-deterministic; sometimes hard-hangs the board at startup
  ("AA doesn't start up stable"), recovers on a power-cycle + dongle re-plug. This is
  the top AA bug to chase (see `HARDWARE_TODO.md` H8). Also: video can lag under heavy
  motion (no frame-drop policy; audio was prioritised).

When picking up: read `AA_PLAN.md` (plan + bench log), `AA_AUDIO_INVESTIGATION.md`
(the audio fix), and `STATUS.md` (module state).
