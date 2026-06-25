# carsh

A bare-metal automotive head unit for the Raspberry Pi 4 (Circle framework +
BTstack): terminal-aesthetic UI, multi-source library, gapless hi-res audio,
parametric EQ, driver profiles, and A/B OTA. See **[PLAN.md](PLAN.md)** for the
full architecture and roadmap — it is the source of truth.

## What this tree is

The application logic is built **interface-first**: hardware sits behind HAL
interfaces (`ISoundDevice`, `IRenderTarget`, `Vfs`/`Source`, `ITouch`,
`IFlashTarget`, `IBluetooth`) so every subsystem runs and is unit-tested on a
build server with **host backends**, with no Pi required:

| Interface | Device backend (Pi) | Host backend (tested here) |
|---|---|---|
| `ISoundDevice` | I2S PCM5122 / PWM | `WavSinkDevice` → WAV |
| `IRenderTarget` | DSI framebuffer | `PngRenderTarget` → PNG |
| `Vfs`/`Source` | USB/SD FAT | `HostVfs` over a real directory |
| `ITouch` | `CRPiTouchScreen` | `SyntheticTouch` event scripts |
| `IFlashTarget` | SD boot slot write | `MemFlashTarget` (RAM) |
| `IBluetooth` | BTstack + CYW43455 | `FakeBluetooth` event driver |

"Done (host)" means it **compiles clean and its host tests pass** — never
"validated on the Pi". Per-module state is in **[STATUS.md](STATUS.md)**; the
bench backlog is **[HARDWARE_TODO.md](HARDWARE_TODO.md)** and open decisions are
in **[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)**.

## Build & test (host)

```sh
cmake -S . -B build
cmake --build build -j
./build/carsh_tests            # run all; pass a substring to filter, e.g. ./build/carsh_tests Decode
```

Requires a C++17 compiler + CMake. Optional dev libs unlock more real tests
(auto-detected): `libopusfile` (Opus decode), `libfaad2` (AAC decode),
`libcrypto`/OpenSSL (real Ed25519 OTA verify). Fixture encoders
(`flac`/`lame`/`opusenc`/`faac`) are used at build time to generate real audio
test vectors; missing ones simply skip the corresponding tests.

## Layout

```
src/        platform-independent core (audio, decode, dsp, library, player,
            profile, ota, ui, bt) + hal/ interfaces
host/       host backends + helpers (WAV/PNG/VFS/touch/flash/bt, OpenSSL crypto)
third_party/ vendored single-headers (dr_libs, stb, font8x8) — built with -w
test/       header-only unit harness (CHECK/REQUIRE/CHECK_NEAR)
tests/      unit tests per module
tools/      gen_fixtures.py (deterministic audio + library fixtures)
```

## Status at a glance

Host-complete: PcmRing/audio engine/gapless, decoders (WAV/FLAC/MP3/Opus/AAC),
PlayerController, library DB/search/smart-playlists/tags/indexer, BS.1770
ReplayGain, cover-art+ASCII, RBJ EQ, profiles, EQ presets, OTA
crypto/package/A-B state machine, the UI toolkit + 7 screens, and the Bluetooth
integration seam. Hardware-gated (bench): DSI panel, touch digitizer, I2S DAC,
real-time/underrun behavior, USB across drives, BTstack/controller, WiFi, and the
OTA flash write + chainloader. The Circle image build itself is H0.
