# Android Auto (wireless, via USB dongle) — implementation plan

Branch: `device/android-auto`. Status tracked at the bottom; this is the working
plan, refined as phases land.

## Goal

Wireless Android Auto on the carsh head unit using a **commercial wireless AA
dongle** (Laser `NAVC-WD25-649`, or the equivalent Response `QM4010`) plugged
into a Pi 4 USB‑A port. The dongle owns *all* the wireless — it pairs the phone
over Bluetooth and runs the 5 GHz WiFi link itself — and presents to the head
unit as a **wired Android Auto phone over USB**. So carsh never touches a radio
for AA; carsh's job is to be a **USB‑host wired Android Auto head unit**.

## Why this is tractable here

- **To the head unit, the dongle ≈ a wired phone.** The wired‑AA interface
  (USB AOA mode‑switch → TLS → protobuf service channels → H.264 video + PCM
  audio + input) is exactly what the reference implementation already drives.
- **Reference code exists and is portable.** `dylant9/head_unit` (ESP32‑P4 /
  ESP‑IDF) implements this whole stack and reached video; most of it is
  platform‑independent C that lifts into `src/aa/`. It already solved the AOA
  identity strings, the framing, and the TLS handshake (mbedTLS — same library
  circle‑stdlib bundles).
- **carsh already has the HAL the AA services need:** `IRenderTarget` (video),
  `ISoundDevice` + `PcmRing` (audio), `ITouch` (input), `CircleUsbLog` (USB‑C
  serial bring‑up log), Circle USB host (`CUSBHostController` control + bulk),
  and bundled **mbedTLS 2.28** for the AA TLS channel.
- **The protocol core is host‑testable.** The captured AA byte streams from the
  reference repo become golden fixtures; the core runs and is regression‑tested
  on the build server with **no hardware**, exactly like the rest of carsh.

## Reference assets to port (from `dylant9/head_unit`)

| Reference file(s) | carsh destination | action |
|---|---|---|
| `aa_transport`, `aa_messenger`, `aa_channels`, `proto/`, `aa_types.h` | `src/aa/` core | **port ~as‑is**, host‑tested |
| `aa_crypto.c` (mbedTLS) | `src/aa/AaCrypto` on circle‑stdlib mbedTLS | small adaptation |
| `aa_sensor`, `aa_control`, `aa_input` | `src/aa/` core | port as‑is |
| `usb_aoa.c` | `device/CircleAoaTransport` (Circle USB host) | **rewrite** on Circle USB; reuse AOA control sequence + identity strings verbatim |
| `aa_video.c` | NAL framing → core; H.264 decode → `device/CircleH264Decoder`; RGB blit → `IRenderTarget` | split + rewrite decode backend |
| `aa_audio.c` | bridge → `ISoundDevice`/`PcmRing` | rewrite thin |
| `aa_proxy_log.bin`, DHU captures | `tests/aa/fixtures/` | **golden test vectors** |
| `aa_ui.c` (LVGL) | drop — use carsh UI + full‑screen video surface | n/a |

## Architecture (interface‑first, mirrors the rest of carsh)

```
src/aa/                         platform-independent AA core (HOST-TESTED vs captures)
  AaTransport / AaMessenger / AaChannels / AaProto / AaCrypto(mbedTLS)
  AaVideoParse (NAL framing) / AaAudioRoute / AaInput / AaControl / AaSensor
  IAaTransport.h    seam: bulk read/write + connect state
  IH264Decoder.h    seam: feed(nal) -> decoded-frame callback
host/aa/
  FakeAaTransport   replays captured AA bytes -> drives the core in unit tests
device/
  CircleAoaTransport   Circle USB host: enumerate dongle -> AOA switch -> bulk IN/OUT   : IAaTransport
  CircleH264Decoder    SW decode on a dedicated A72 core (CMultiCoreSupport)            : IH264Decoder
  bridges: AA audio -> ISoundDevice ring · AA video frame -> IRenderTarget · touch -> AaInput
```

In the app, AA is a **source/mode** like BT or USB media: when active, route AA
audio into the engine and present the AA video surface full‑screen with touch
forwarded back to the phone.

## Staged plan (de‑risk order)

`[now]` = no hardware required (do before the dongle arrives); `[hw]` = needs the
dongle and/or the board.

**Phase 0 — scaffolding** `[now]`
- `src/aa/` skeleton, the two interface seams, `tests/aa/` harness, STATUS entry.

**Phase 1 — protocol core, host‑tested** `[now]` — *the bulk of the work*
- Port transport / messenger / channels / proto / crypto into `src/aa/`.
- Bring captured AA streams in as fixtures; `FakeAaTransport` replays them.
- Host tests assert the state machine reaches each milestone: framing →
  version exchange → TLS handshake (mbedTLS, offline vs captured certs) →
  service discovery → channel routing → first AV/control message parsed.

**Phase 2 — H.264 decode** `[now]` correctness / `[hw]` performance
- NAL framing host‑tested `[now]`; decode a captured stream and checksum frames
  `[now]`; sustained on‑device FPS at 800×480 on a dedicated A72 core `[hw]`.

**Phase 3 — USB‑AOA device backend** `[hw]` — *highest risk, spike standalone*
- `CircleAoaTransport`: enumerate the dongle, run the AOA mode‑switch (reuse the
  reference's exact identity strings — Google's AA allowlist checks them),
  re‑enumerate to `18d1:2d0x`, open bulk IN/OUT, and stream raw bytes to the
  USB‑C serial log **with a throughput counter**. This is where the ESP32 hit a
  USB wall — different silicon/stack here, but *prove sustained bulk‑IN MB/s
  before anything else.*

**Phase 4 — integration** `[hw]`
- Wire `CircleAoaTransport` + `CircleH264Decoder` into the core; video →
  framebuffer, audio → engine ring, touch → input channel. First projection.

**Phase 5 — polish** `[hw]`
- AV sync / flow‑control, audio focus, disconnect/reconnect, optional mic
  uplink for voice. (Voice can be deferred.)

## Hardware notes

- Dongle in a Pi 4 **USB‑A** port (xHCI host). Coexists with the separate BT
  A2DP adapter and the USB media stick (all on xHCI); the USB‑C port stays the
  CDC serial‑log gadget.
- Phone needs **Android 11+** (wireless‑AA requirement, dongle‑side).
- Cheap de‑risk before the Pi port: bench‑test the dongle against the ESP32
  `head_unit` rig — if it drives the dongle like a phone, it's validated.

## Risks

1. **SW H.264 decode FPS** on the A72 (Phase 2/4) — the standout. Mitigation:
   dedicate a core; 800×480 is modest; the far‑weaker P4 managed SW decode.
2. **USB bulk throughput on Circle/xHCI** (Phase 3) — the ESP32's nemesis;
   prove it early and in isolation.
3. **Dongle quirks vs a non‑certified head unit** — only the bench tells;
   reuse `head_unit`'s solved AOA strings and compare against its captures.
4. **mbedTLS cipher coverage** — the AA handshake in the DHU capture is
   `TLSv1.2 ECDHE‑RSA‑AES128‑GCM‑SHA256`; confirm that suite is enabled in the
   circle‑stdlib mbedTLS config.

## Test strategy

Capture‑replay. The AA byte captures become golden fixtures; `FakeAaTransport`
feeds them to the core; host tests assert the state machine. This makes
essentially all of Phase 1–2 regression‑tested on the build server with no
radio — the same discipline as the rest of carsh ("host‑done" = compiles +
host tests pass, never "validated on the Pi").

## Status

- [x] **Phase 0** — branch + plan; `src/aa/` scaffold; `IAaTransport` seam;
  `host/aa/FakeAaTransport`; `tests/aa/` harness + capture fixtures.
- [x] **Phase 1** — protocol core, host-tested:
  - [x] `AaMessenger` framing/reassembly — byte-exact vs the real DHU capture.
  - [x] `Pb` protobuf wire codec.
  - [x] `IAaCrypto` TLS seam + `FakeAaCrypto`.
  - [x] `AaConnection` bring-up state machine (version → SSL handshake → auth →
        service discovery → channel opens → dispatch), driven end-to-end by a
        scripted phone in host tests.
  - [ ] *Remaining (breadth):* per-channel handlers (av/sensor/input/media/nav
        decode — currently routed to a generic callback); the real mbedTLS-backed
        `IAaCrypto` for the device (live handshake is `[hw]`).
- [x] **Phase 2** — H.264 NAL extraction/classification (`H264Nal`) +
  `IH264Decoder` seam, host-tested.
  - [ ] *Remaining `[hw]`:* the SW decoder impl + on-target FPS.
- [ ] **Phase 3** — `CircleAoaTransport` USB-AOA spike (needs the dongle).
- [ ] **Phase 4** — integration / first projection.
- [ ] **Phase 5** — polish.

The whole protocol spine — framing, protobuf, the full connection handshake, and
video NAL parsing — now builds and is regression-tested on the build server with
no radio, against Google's real DHU capture bytes where possible (143 host tests).
What's left is breadth (more per-channel decoders) or genuinely hardware-gated
(live TLS against the dongle, the USB-AOA transport, on-target decode FPS).
