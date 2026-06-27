# Android Auto (wireless, via USB dongle) — implementation plan

Branch: `device/android-auto` (Phases 0–2, **merged to main** PRs #9/#10).
Hardware bring-up continues on `device/aa-dongle-bringup`. Status tracked at the
bottom; this is the working plan, refined as phases land.

## Goal

Wireless Android Auto on the carsh head unit using a **commercial wireless AA
dongle** — the owner's actual device is a **MINIX CA525** ("NEO" wireless
CarPlay+AA adapter) — plugged into a Pi 4 USB‑A port. The dongle owns *all* the
wireless — it pairs the phone over Bluetooth and runs the 5 GHz WiFi link itself —
and presents to the head unit as a **wired Android Auto phone over USB**. So carsh
never touches a radio for AA; carsh's job is to be a **USB‑host wired Android Auto
head unit**.

> **Dongle is known-good.** The CA525 has already been paired to the owner's phone
> successfully in another (certified) car — so the wireless/phone side works; the
> only unknown is the carsh USB-host wired-AA side. The bring-up ladder below
> isolates exactly that.

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
  Present AA inside one UI tab (see "UI integration" below), not full-screen.

**Phase 5 — polish** `[hw]`
- AV sync / flow‑control, audio focus, disconnect/reconnect, optional mic
  uplink for voice. (Voice can be deferred.)

## Hardware bring-up ladder — the actual remaining work (CA525)

The protocol spine is done and merged. Everything below is hardware, sequenced
**strictly by de-risk order** — each rung proves one unknown in isolation over the
USB-C serial log before the next is attempted. This reuses the exact bench loop
that cracked BT A2DP: flash SD → `run_in_background` serial capture on
`/dev/ttyACM0` → power-cycle → read the log → iterate. **Build a rung's diagnostic,
read the serial, only then write the next rung.**

**Rung 1 — Enumerate the dongle `[hw, cheapest first light]`.**
No AOA yet. On boot, walk Circle's USB device tree and dump every descriptor for
the CA525 to the serial log: initial VID/PID, device/interface class, configs,
endpoint map (are bulk EPs present pre-switch?), whether it's a composite/hub.
*Answers:* what mode the dongle powers up in, and what we're switching *from*.
Likely starts as a vendor/CDC-ish device with no AA bulk pipes until AOA runs.

**Rung 2 — AOA mode-switch `[hw, the identity handshake]`.**
On the dongle's control endpoint, run the Android Open Accessory sequence
(reuse `head_unit`'s **exact** strings — Google's AA allowlist matches
manufacturer/model):
1. control-IN `bRequest=51` (GET_PROTOCOL) → AOA version (expect ≥1).
2. control-OUT `bRequest=52` (SEND_STRING) ×6 — manufacturer/model/desc/version/
   uri/serial. The AA-launching pair is manufacturer=`Android`, model=`Android Auto`.
3. control-OUT `bRequest=53` (START) → dongle detaches.
4. It re-enumerates as **`18d1:2d0x`** (Google, PID 0x2d00–0x2d05) exposing two
   bulk endpoints. *Circle wrinkle:* handle the detach→re-attach and bind the
   transport to the new device's bulk IN/OUT (device-add callback or rescan).
*Answers:* does Circle's xHCI host drive AOA + survive the re-enumeration.

**Rung 3 — Bulk throughput spike `[hw, the ESP32's nemesis]`.**
Before any protocol: blast raw bulk-IN and print sustained **MB/s** to serial.
Need ~2–5 MB/s for 480p30 H.264 + PCM. *Partly pre-de-risked:* xHCI bulk already
streams a FAT media stick on this board, so the controller works — but the AA bulk
pattern (large, back-to-back IN transfers) is what killed the ESP32; prove it here
in isolation. If it can't sustain, nothing downstream matters.

**Rung 4 — Real TLS, live handshake `[hw]`.**
Swap `FakeAaCrypto` for an mbedTLS-backed `IAaCrypto` (port `head_unit/aa_crypto.c`;
circle-stdlib bundles mbedTLS 2.28). Confirm the suite the DHU capture uses —
`TLSv1.2 ECDHE-RSA-AES128-GCM-SHA256` — is enabled in the mbedTLS config. Feed
`CircleAoaTransport` into the existing `AaConnection`; the already-tested state
machine should drive version → **live TLS** → auth → service discovery → channels.
*Answers:* does the merged core reach service discovery against a real phone.

**Rung 5 — H.264 decode + display `[hw, the standout risk]`.**
Decision: **software decode** — bare-metal Circle has *no* path to the VideoCore
HW decoder (that needs VCHIQ/MMAL, i.e. Linux), so HW decode is off the table by
the [[no-linux-bare-metal]] constraint. Plan:
- **Decoder: openh264** (Cisco, BSD-2, Constrained-Baseline, has NEON) on a
  **dedicated A72 core** via Circle `CMultiCoreSupport`. AA video is
  Constrained-Baseline, so a full profile isn't needed.
- Keep the advertised **480p (800×480)** video config (we *own* the service-
  discovery video params, so we cap what the phone sends). Decode YUV → NEON
  YUV→RGB → scale-blit into `contentRect()` (800×480 → 696×432, ~0.87×).
- **Measure on-target FPS first** — this is the one number that can force a
  fallback (lower fps in the video config, or split decode across both spare
  cores). 800×480 CB on an A72+NEON is feasible but tight; treat FPS as a gate.

**Rung 6 — Integration `[hw]`.** Wire it together behind the existing seams: AA
video frame → `IRenderTarget` (new "Android Auto" 9th tab, see UI section), AA
audio PCM → `ISoundDevice`/`PcmRing`, content-rect touch → geometry remap to
800×480 → AA input channel, `VIDEO_FOCUS_INDICATION` tied to the active tab.

**Rung 7 — Polish `[hw]`.** AV sync / flow-control, audio focus, disconnect/
reconnect, optional mic uplink (voice deferrable). = Phase 5.

*Host-side work I can do now without the dongle:* scaffold `CircleAoaTransport`
(IAaTransport impl: AOA control sequence + bulk wrappers, compiled against Circle
USB but bench-validated later), port `aa_crypto.c` to an mbedTLS `IAaCrypto`, add
the `openh264` decode backend behind `IH264Decoder` with a host frame-checksum
test, and build the AndroidAuto screen + touch-remap geometry (host-testable). The
hardware rungs above then validate each in turn.

## UI integration — AA as a tab, chrome stays visible (Phase 4)

Owner requirements: the AA video feed lives **entirely within one UI tab** (so the
user can switch to other tabs while AA keeps running), and the **left volume rail +
tab bar stay visible** without the video covering them.

This maps onto the existing UI cleanly — carsh's chrome is already fixed outside
the content area:
- `kRailW = 52` px left volume rail, `kTabH = 48` px full-width bottom tab bar.
- `Screen::contentRect()` = `{52, 0, w-52, h-48}` → `{52, 0, 748, 432}` on the
  800×480 panel, which **already excludes both**. So an "Android Auto" screen that
  blits the decoded frame into `contentRect()` keeps the rail + tabs visible by
  construction, and tab-switching is unchanged.

Approach when Phase 4 lands:
1. **New `AndroidAuto` screen** (the 9th tab — 8 already exist incl. BT
   scan/connect). Its draw blits the latest decoded AA frame into `contentRect()`;
   everything else stays standard chrome. The volume rail + tab bar that frame it
   are already **hw-validated** on the Pi 4, so this confines cleanly by design.
2. **Resolution + scale.** AA video sizes are fixed standards (480p = 800×480,
   720p, 1080p) — you can't request 696×432. Keep the already-advertised **480p**
   (service-discovery video config), decode 800×480, and **scale-blit** into the
   696×432 content rect (~0.87×, a cheap, good-quality downscale). Map touches in
   the content rect back to 800×480 for the AA input channel (pure geometry —
   host-testable when wired).
3. **Tie video focus to the active tab.** Send `VIDEO_FOCUS_INDICATION` FOCUSED
   when the AA tab becomes active and UNFOCUSED when the user switches away, so the
   phone isn't streaming frames we're not showing. (Reference:
   `build_video_focus_indication`.)

Not done now: needs the live decoder + framebuffer compositor + an active
connection (all Phase 4). Captured here so it's built right; the design requires
no change to the protocol core already written.

## Hardware notes

- **The platform under AA is already hardware-validated** (main, June 2026): the
  base image boots on a real Pi 4 as a working media player — DSI framebuffer
  (`IRenderTarget`), touch (`ITouch`), PWM-jack audio (`ISoundDevice`), USB-host
  FAT media (xHCI), and the USB-C serial console are all confirmed on the board.
  So every backend AA Phase 4 plugs into is proven, not assumed; bring-up debug
  has a live serial channel. (See STATUS.md / HARDWARE_TODO H0–H5.)
- Dongle in a Pi 4 **USB‑A** port (xHCI host). Coexists with the separate BT
  A2DP adapter and the USB media stick (all on xHCI); the USB‑C port stays the
  CDC serial‑log gadget.
- Phone needs **Android 11+** (wireless‑AA requirement, dongle‑side).
- Cheap de‑risk before the Pi port: bench‑test the dongle against the ESP32
  `head_unit` rig — if it drives the dongle like a phone, it's validated.

## Risks

1. **SW H.264 decode FPS** on the A72 (Phase 2/4) — the standout. Mitigation:
   dedicate a core; 800×480 is modest; the far‑weaker P4 managed SW decode.
2. **USB bulk throughput on Circle/xHCI** (Phase 3) — the ESP32's nemesis.
   *Partly de-risked:* the xHCI host + bulk transfer are now hw-validated on the
   board (a FAT media stick mounts + streams), so the controller works; AA adds
   AOA control + a custom bulk device on that proven host. Still prove sustained
   bulk-IN throughput early and in isolation.
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
  - [ ] *Remaining `[hw]`:* the SW decoder impl (openh264) + on-target FPS.
- [~] **Phase 3 / rungs 1-3** — `CircleAoaTransport` USB-AOA backend **scaffolded**
  (`device/CircleAoaTransport.{h,cpp}`): compiles + links into the Pi 4 image. Has
  the full AOA mode-switch sequence (reqs 51/52/53 + AA identity strings), the
  Rung-1 descriptor dump, blocking bulk-OUT, and async bulk-IN → SPSC ring → `read()`
  (the proven CUSBBluetoothDevice/CircleHciTransport pattern), plus the Rung-3
  throughput counter. *Remaining `[hw]`:* device-acquisition wiring (depends on how
  the CA525 enumerates — Rung 1 on the bench) + on-board throughput numbers.
- [x] **UI integration (Phase 4, host-side)** — `AndroidAutoScreen` (the "AA" tab)
  + `AaVideoGeometry` (aspect-fit + touch→source remap) + Screen focus callbacks,
  host-tested, registered in `kernel.cpp`. The video surface + chrome layout is done;
  it shows a placeholder until the decoder + transport feed it.
- [ ] **Phase 4 `[hw]`** — wire transport + decoder into `AaConnection`; first projection.
- [ ] **Phase 5** — polish.

The whole protocol spine — framing, protobuf, the full connection handshake, video
NAL parsing — plus the AA UI surface and the USB-AOA transport scaffold now build
(160 host tests; the Pi 4 image links with the AA tab + transport). What's left is
hardware-gated: the live USB-AOA bring-up (rungs 1-4), the openh264 decoder + FPS
(rung 5), and final integration (rung 6).
