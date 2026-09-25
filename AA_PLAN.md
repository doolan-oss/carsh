# Android Auto (wireless, via USB dongle) — implementation plan

Branch: `device/android-auto` (Phases 0–2, **merged to main** PRs #9/#10).
Hardware bring-up merged via PRs #11/#12. Status tracked at the bottom; this is the
working plan, refined as phases land.

---

## Autonomous execution brief (read this first if you are an agent)

**Mission.** Carry the Android Auto pipeline forward from where hardware bring-up
stopped (rung 4a, version exchange working on the board) **through to the video
pipeline** — TLS, channel decoders, H.264 decode, and integration into the existing
`AndroidAuto` UI tab. Implement rungs **4b → 6** below.

**The one hard boundary: you have NO hardware.** You cannot flash an SD card, attach
the dongle, or read the serial console. So:
- Deliver everything to **"host-green + ready to flash"**: `cmake` host build passes,
  `./build/carsh_tests` is green, and `cd device && make` cross-compiles + links
  `kernel8-rpi4.img` clean. **Compile-check the device image after every device-side
  change** (it catches real Circle API mistakes — see the bench log below).
- **Never claim anything is hardware-validated.** In `STATUS.md`/`AA_PLAN.md`, mark
  your work `host-done` or `builds`, never `hw-validated`. The owner does the bench
  pass that promotes it.
- Anything whose correctness genuinely needs a live phone/dongle (the TLS handshake
  completing, on-target decode FPS, sustained bulk throughput) — build it behind its
  interface, **host-test what you can against fixtures, and leave a clear
  `[hw]`-tagged validation note** for the bench. Do not fake a pass.

**Definition of done, per rung** (technical detail is in the rung sections below):
- **4b — TLS.** `device/AaCryptoMbedTls.{h,cpp}` implements `IAaCrypto` (port
  `dylant9/head_unit:main/aa_crypto.c`). The four mbedTLS `.a`s + the config define
  link into the image. Add a host test that drives it against a captured handshake
  **if** a fixture exists in `tests/aa/fixtures/`; otherwise host-test the BIO/round
  -trip plumbing and tag the live-handshake check `[hw]`. Swap `LoggingCrypto` →
  `AaCryptoMbedTls` in `CircleAaDriver`.
- **4c — per-channel handlers.** Flesh out `AaConnection`'s service discovery +
  channel-open + dispatch for video / media-audio / input (today some route to a
  generic callback). Host-test against the DHU capture fixtures, same discipline as
  `AaConnection_test`.
- **5 — H.264 decode.** Vendor + build **openh264** (BSD, Constrained-Baseline) for
  aarch64 bare-metal; implement `IH264Decoder` behind the existing seam. **Host-test:
  decode a captured Annex-B clip and checksum frames** (this is genuinely host-doable;
  `H264Nal` parsing is already host-tested). On-target FPS is `[hw]`.
- **6 — integration.** Bridge decoded frames → `AndroidAutoScreen` (already built:
  aspect-fit blit + touch remap in `src/ui/AndroidAutoScreen.h`), AA audio → an
  `ISoundDevice`/`PcmRing`, content-rect touch → AA input channel,
  `VIDEO_FOCUS_INDICATION` ↔ the active tab. Host-test the wiring with fakes.

**Guardrails.**
- Branch off `main` (e.g. `device/aa-video-pipeline`). **Do not commit to or push
  `main`.** Open a PR when a rung is done; keep PRs reviewable.
- `third_party/circle-stdlib/` is **git-ignored** — never commit into it. Any edit to
  it is a tracked patch under `scripts/patches/` + a README entry (mirror the existing
  ones). mbedTLS is built with `cd third_party/circle-stdlib && make mbedtls`.
- Keep `./build/carsh_tests` green at every step; add tests for new core logic.
- Read `CLAUDE.md` (build/bench/constraints), then the rung sections + bench log
  below. Update the Status section as you land each rung.

**If you get stuck on something only hardware can answer:** stop, write down the exact
question + what you'd need captured (e.g. "need a real TLS handshake byte capture as a
fixture"), and leave it for the bench rather than guessing.

---

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
  YUV→RGB → scale-blit into `contentRect()` (800×480 → 720×432 at (66,0) inside the
  748×432 content rect, 0.9×).
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
   720p, 1080p) — you can't request 748×432. Keep the already-advertised **480p**
   (service-discovery video config), decode 800×480, and **scale-blit** into the
   748×432 content rect (height-bound fit: 720×432 at (66,0), 0.9×, a cheap,
   good-quality downscale). Map touches in
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
  - [x] *Per-channel handlers (rung 4c, 2026-06-28):* `AaConnection` now dispatches
        the Running phase into structured handlers instead of one generic callback —
        AV setup/start/stop + media (timestamp-stripped H.264/PCM via `onAvMedia`)
        with `AV_MEDIA_ACK` flow control, input binding + outbound touch
        (`sendTouch`), sensor start (+ driving-status/night event), and video focus
        (`onVideoFocusRequest` / `sendVideoFocus`). Host-tested in
        `tests/aa/AaChannels_test.cpp` (scripted phone, protobuf decoded back). Nav /
        media-status still fall through to `onChannelMessage`. Wire acceptance by a
        real phone is `[hw]`.
  - [x] *Real mbedTLS-backed `IAaCrypto`* — done in rung 4b (host-verified; live
        handshake `[hw]`).
- [x] **Phase 2** — H.264 NAL extraction/classification (`H264Nal`) +
  `IH264Decoder` seam, host-tested.
  - [x] *SW decoder impl (rung 5, 2026-06-28):* `device/CircleH264Decoder` on
        openh264 (Cisco, BSD, Constrained-Baseline) + `src/aa/Yuv420.h` (I420→RGB888,
        BT.601). The host build links system openh264 2.4.1 (the line vendored for
        the Pi) and runs a real **encode→decode→pixel-compare** round trip
        (`tests/aa/H264Decode_test.cpp`, MAE-bounded so it proves a genuine decode),
        plus converter anchors. Device gate: `make CARSH_HAVE_OPENH264=1` +
        `scripts/fetch_openh264.sh`.
  - [ ] *Remaining `[hw]`:* the aarch64 bare-metal openh264 cross-build, on-target
        FPS at 800×480, and running `feed()` on a dedicated A72 core.
- [~] **Phase 3 / rungs 1-3** — `CircleAoaTransport` USB-AOA backend, **bench bring-up
  in progress** (`device/CircleAoaTransport.{h,cpp}` = `CAndroidAutoDevice`, a
  `CUSBFunction` + `IAaTransport`; bound via `scripts/patches/usbdevicefactory-aoa.patch`).
  See the bench-session findings below.

### Bench session — 2026-06-28 (CA525 + Galaxy S23 Ultra on real Pi 4)

Picked up at rung 4a (plaintext version exchange) and drove the **whole protocol live
to full channel setup**. Loop: serial `rx frame`/`tx frame` traces (added this session)
→ each failure pinpointed a protocol gap → fix → reflash. Sequence of fixes, each
advancing the live session one stage deeper:

1. **Real cert + mbedTLS threading (rung 4b TLS — DONE live).** Swapped the placeholder
   for the real aasdk cert (`AaHuCert.h`). The handshake still failed with mbedTLS
   `-0x001c` (`THREADING_BAD_INPUT_DATA`) and *no ClientHello on the wire* — root cause:
   circle-mbedtls is built `MBEDTLS_THREADING_C`+`THREADING_ALT`, so `mbedtls_mutex_lock`
   is a fail-stub until `mbedtls_threading_set_alt()` is installed; carsh drives raw
   mbedTLS and never installed it (`CTLSSimpleSupport` would have). Fix: construct a
   `CircleMbedTLS::CThreadingModule` once in `CircleAaDriver` before any context. → **TLS
   handshake, auth, and service discovery now complete on hardware; state reaches
   Running.**
2. **Channel-open routing.** Phone sends `CHANNEL_OPEN_REQUEST (0x0007)` on the *target*
   channel, not Control; carsh dropped them → phone aborted right after discovery. Route
   by msg id regardless of channel; frame channel is authoritative; reply on it. (matches
   `head_unit:aa-fixes`.)
3. **Audio/Nav focus.** Added `AUDIO_FOCUS_REQUEST (0x12)` / `NAV_FOCUS_REQUEST (0x0D)`
   handlers — the phone blocks on the audio-focus response after discovery.
4. **Message *type* of responses.** AV setup / sensor-start / input-binding **responses
   must be `MessageType::Control`**, not Specific — a Specific SETUP_RESPONSE is treated
   as stray media and the phone never starts AV. Media-acks / focus-indications / events
   stay Specific. Also: proactively send `VIDEO_FOCUS_INDICATION(FOCUSED)` after Video
   setup; SD-encoding fixes (AVInputChannel stream_type/audio_config, VideoFPS `_30=1`).
5. **Drain the whole burst per tick** (`CircleAaDriver`): the phone fires ~12-20
   open/setup messages in ~20 ms; answering only 8/tick spread replies past its timeout.

Result: **all 8 channels open and every setup message is answered with the phone
accepting them** (verified by the `tx frame` trace) — the AA protocol is essentially
complete on hardware.

**Open blocker — the bulk nemesis (risk #2).** ~0.1-3 s into Running, *during the
open/setup burst*, the bulk-IN throws `xhciep: Transfer error 4` (USB completion-code-4
/ `USBErrorUnknown`) and the dongle hard-resets to PTP and re-runs AOA (phone: "comms
error 2"). The failure point is **non-deterministic** across runs (channel-opens →
AV-setup → 3 s idle), which rules out a deterministic protocol bug — it's a
timing-sensitive USB/xHCI issue. Tried and ruled out: **async bulk-OUT** (regressed
response latency, reverted), **double-buffered bulk-IN** (2 URBs always posted — no
change, kept), **IN endpoint recovery** (the dongle genuinely re-enumerates; removal is
just detected ~1 s later by the next `UpdatePlugAndPlay`) (2026-09-25 review A3: the
carsh re-post path could never run — a halted xHCI endpoint never completes the second
URB — and re-posting onto a halted ring does nothing; replaced by the dead-link latch +
forced re-enumeration). The reference hit analogous
"bulk issues" and solved them with **ESP-IDF-specific** USB-FIFO/pacing tweaks that don't
translate to the Pi's xHCI.

Dongle sanity-checked on a Linux PC (MTP `smartlinkBox/UDISK`): link type = AA, paired to
"Dylan's S23 Ultra", a `1280x720-…h264` asset (the dongle is a **720p** device; carsh
advertises 480p — fine for channel-open per the reference, revisit for streaming). So the
dongle is healthy; the blocker is Circle-side.

*Next options:* (a) build **OpenAuto** (aasdk) on the PC + `usbmon` capture — definitive
"Circle vs dongle" answer and a golden bulk trace to diff; (b) Circle-xHCI deep-dive on
the bulk-IN/OUT path under burst. All this session's code changes are in the working tree
(uncommitted on `main`).

### Bench session — 2026-06-27 (CA525 on real Pi 4)

Methodology: the BT bring-up loop (flash SD → `run_in_background` capture on the USB-C
console → power-cycle → read log). Key change that unblocked it: bring the USB-C
console up **before** the xHCI host inits, so Circle's per-device enumeration log is
captured (it scrolls past on the GPIO UART otherwise), plus pump `UpdatePlugAndPlay()`
at ~1 Hz so a slow-booting dongle is seen.

- **Rung 1 — enumerate `[DONE]`.** The CA525 powers up (slowly, ~2 s after a media
  stick would) as **`ven2717-ff40` "Android MINIX", interface `int6-1-1` (PTP)**.
  Circle has no PTP driver, so it's unbound; with no AOA handshake the dongle
  disconnects and retries every ~16 s.
- **Rung 2 — AOA switch `[DONE]`.** The factory hook binds `CAndroidAutoDevice` to
  `ven2717-ff40`; its `Configure()` runs the AOA sequence (probe returns **AOA v2**),
  and the dongle **re-enumerates in accessory mode as `ven18d1-2d00`**. Fixed on the
  way: xHCI DMA asserts on `.rodata` buffers, so the AOA identity strings must be
  copied to heap buffers before the control transfers.
- **Rung 3 — bind accessory bulk `[DONE]`.** The accessory device was being
  `Configure()`-skipped by a Circle hub bug: after our pre-switch `Configure()`
  returns FALSE the hub deletes the device but leaves its per-port
  `m_bPortConfigured` flag set, so the replacement (accessory) device on the same
  port was never configured. Fix = `scripts/patches/usbstandardhub-reconfigure.patch`
  (clear the flag on teardown). **Verified:** `configureAccessory` binds two 512-byte
  HS bulk endpoints (IN `0x81` / OUT `0x01`), registers `uaa1`, and
  `CAndroidAutoDevice` is the live `IAaTransport`. (It also unregisters on
  disconnect so the link rebinds across the dongle's connect cycles.)
- **Rung 4a — plaintext version exchange `[DONE]`.** `CircleAaDriver` drives an
  `AaConnection` over `uaa1` from the UI loop. On hardware with a phone live on the
  dongle: `VERSION_REQUEST` → the phone replies `VERSION_RESPONSE` (status OK,
  rx=12 B), state advances to `SslHandshake`. **The bulk path is bidirectional and
  the AA protocol is live end-to-end.** A logging stub crypto stops it there.
- **Rung 4b — TLS `[next, fully scoped + de-risked 2026-06-28]`.** The phone now
  expects a TLS handshake. All unknowns resolved this session:
  - **mbedTLS builds for bare-metal Circle.** `cd third_party/circle-stdlib && make
    mbedtls` produces `libs/mbedtls/library/libmbed{crypto,x509,tls}.a` +
    `src/circle-mbedtls/libcircle-mbedtls.a` (the glue — entropy via Circle's
    `CBcmRandomNumberGenerator`, threading, config). **Link recipe for
    `device/Makefile`:** add those four `.a` (crypto last — link order
    tls→x509→crypto) to `LIBS`; add `-I.../include -I.../libs/mbedtls/include` and
    `-DMBEDTLS_CONFIG_FILE='<circle-mbedtls/config-circle-mbedtls.h>'` to the AA
    crypto unit's flags.
  - **Crypto + cert reference: `dylant9/head_unit:main/aa_crypto.c`** (saved notes
    below). It's a complete mbedTLS impl that maps 1:1 to `IAaCrypto`. Port to
    `device/AaCryptoMbedTls.{h,cpp}`:
    - **HU = SSL client** (`MBEDTLS_SSL_IS_CLIENT`, stream, preset default), RNG =
      ctr_drbg seeded from the entropy module, **`authmode = VERIFY_NONE`** (AA is
      self-signed), `ssl_conf_own_cert(cert, pkey)`, `ssl_set_bio` → in-memory BIO.
    - **In-memory BIO** (16 KB buffers): `bio_send` appends to an out-buffer,
      `bio_recv` drains an in-buffer (returns `WANT_READ` when empty). `handshakeStep`
      = push `in` → `mbedtls_ssl_handshake()` (tolerate `WANT_READ/WRITE`) → return
      the out-buffer. `encrypt` = `ssl_write` → out-buffer; `decrypt` = push cipher →
      `ssl_read`.
    - **Cert = the aasdk / JVC-Kenwood HU certificate** (registered with Google's AA
      program; **GPLv3** — note provenance). It's embedded in `aa_crypto.c` as
      `aa_cert_der_fallback[]` (raw DER) + `aa_private_key_pem_fallback` (RSA PEM).
      Self-signed will NOT work — the phone requires this specific cert.
    - **Critical gotcha:** the cert uses non-standard UTCTime (`-0700` offset) that
      mbedTLS **rejects** — bypass x509 parsing: set `cert.raw.p/len` to the DER,
      `cert.version = 3`, `own_buffer = 0`, skip `x509_crt_parse`. Parse the key
      normally with `mbedtls_pk_parse_key`.
  - Then swap `LoggingCrypto` → `AaCryptoMbedTls` in `CircleAaDriver`; the state
    machine continues handshake → auth → service discovery → channel opens → first AV
    (rungs 5-6 = openh264 decode + the AndroidAuto tab already built).
  - **[DONE host-side 2026-06-28]** `device/AaCryptoMbedTls.{h,cpp}` implements
    `IAaCrypto` on mbedTLS exactly as scoped above: HU = TLS 1.2 client, suite pinned
    to ECDHE-RSA-AES128-GCM-SHA256, `VERIFY_NONE`, ctr_drbg seeded from an injected
    entropy fn (Circle HW RNG on device), in-memory BIO (`src/aa/AaMemBio.h`, pure +
    host-tested), and the raw-cert bypass for the aasdk cert's non-standard UTCTime
    (`loadOwnCert`). The in-memory BIO + the full handshake + encrypt/decrypt
    round-trip are **host-verified against system mbedTLS 2.28** — the same 2.28 line
    circle bundles — so the plumbing the phone exercises is proven on the build
    server (`tests/aa/AaCryptoMbedTls_test.cpp`, incl. forcing the raw-cert path).
    `CircleAaDriver` now constructs `AaCryptoMbedTls` (HW-RNG entropy) instead of
    `LoggingCrypto`. `device/Makefile` links the four mbedTLS archives (grouped) with
    the `MBEDTLS_CONFIG_FILE=<circle-mbedtls/config-circle-mbedtls.h>` define.
    *Still `[hw]`:* (a) the device link itself (`cd device && make`) — not runnable in
    the agent env (no cross-toolchain / circle-stdlib checkout); the unit is
    host-compile-verified against the identical mbedTLS API. (b) The **live phone
    handshake**, which needs the real Google-registered aasdk head-unit cert dropped
    into `device/AaHuCert.h` (currently a clearly-labelled placeholder test cert) +
    confirming the circle-mbedtls config enables the pinned suite (risk #4).
- [x] **UI integration (Phase 4, host-side)** — `AndroidAutoScreen` (the "AA" tab)
  + `AaVideoGeometry` (aspect-fit + touch→source remap) + Screen focus callbacks,
  host-tested, registered in `kernel.cpp`. The video surface + chrome layout is done;
  it shows a placeholder until the decoder + transport feed it.
- [~] **Phase 4 / rung 6 — integration (host-side done 2026-06-28).**
  `src/aa/AaProjection` ties the live `AaConnection` to the decoder, the AA tab, and
  audio: video media → `IH264Decoder::feed` → decoded frame → `AndroidAutoScreen::
  setFrame`; audio media → a PCM hook; channel-open/shutdown → `setConnected`;
  raw touch (Press on down / Drag on move / Release on up) → `sendTouch` (input
  channel); AA-tab focus → `sendVideoFocus`
  (`VIDEO_FOCUS_INDICATION`). Host-tested end to end with fakes
  (`tests/aa/AaProjection_test.cpp`). `CircleAaDriver` binds the projection to each
  connection across dongle cycles; `device/kernel.cpp` provides the real hooks +
  `CircleH264Decoder` (under `CARSH_HAVE_OPENH264`). *Remaining `[hw]`:* the live
  end-to-end projection on the board, and draining the AA audio ring to the jack with
  audio focus (rung 7). The device link itself is bench-only here (no toolchain).
  (Both have run on the bench since 2026-06-29 — see the log below.)
- [~] **Phase 5 / rung 7** — polish: AV sync / flow-control tuning, audio focus +
  draining AA PCM to the jack (AA media audio plays on the bench since 2026-06-29),
  and:
  - [x] *raw touch (drag) streaming* — host-done 2026-09-25: the AA tab's raw path
    (ScreenManager raw-touch opt-in) sends PRESS on down, DRAG per move, RELEASE on
    up, from a 100 Hz touch pump with 64-bit capture timestamps; `sendTouch` now uses
    the aasdk layout (the old encoding never reached the phone). Primary pointer only
    — multi-pointer (POINTER_DOWN/UP + action_index) is future work. `[hw]`
    HARDWARE_TODO H9 §A.
  - [x] *disconnect/reconnect* — host-done 2026-09-25: `AaProjection::disconnect()`;
    Shutdown on SHUTDOWN_REQUEST (acknowledged) or a TLS failure; `AaLinkSupervisor`
    ends the session and re-enumerates the dongle if it doesn't re-cycle. `[hw]` H9 §B.
  - [ ] *optional mic uplink* — the AV-input channel is answered (OK) but no uplink is
    sent, so voice sessions time out on silence.

The whole protocol spine — framing, protobuf, the full connection handshake, video
NAL parsing — plus the AA UI surface and the USB-AOA transport scaffold now build
(160 host tests; the Pi 4 image links with the AA tab + transport). What's left is
hardware-gated: the live USB-AOA bring-up (rungs 1-4), the openh264 decoder + FPS
(rung 5), and final integration (rung 6).

---

## Bench bring-up log — 2026-06-29 (rungs 1–7 working; audio "good enough")

Full AA now runs on the real Pi 4 + CA525 + phone: enumerate → AOA → `uaa1` → version
→ live TLS → service discovery → channels → **H.264 video + media audio both play**,
and AA media is wired into the player UI (`player::MediaSession`: Now Playing source
badge + AVRCP-style transport keys over the input channel).

**Rung 7 (choppy audio) — FIXED, branch `claude/aa-decode-core` / PR #24.** Root-caused
on the bench with new serial instrumentation (`aa-audio:` ring/under/prime/render-µs,
`aa-decode:` n/dec-µs/queue/dropped). The chain:
1. 720p software H.264 decode is **~37 ms/frame > the 33 ms real-time budget**. On one
   core it starved the core-0 audio refill (continuous underruns) AND backed up the AU
   queue → dropped keyframes → **screen froze** until the next IDR.
2. Moved decode to a **dedicated A72 core** (`CircleAaDecodeCore`, `CMultiCoreSupport`,
   `ARM_ALLOW_MULTI_CORE` — rebuilt libcircle). Frames cross to the UI core via a
   lock-free triple buffer (`FrameRelay`); audio (`g_aaRing`) stays on core 0.
3. **Dropped video to 480p (800×480 — the panel's native size).** Decode → ~15 ms
   (real-time, `dropped=0`, freeze gone), less memory-bus contention. AA video
   resolution enum: `_480p=1` (`AaConnection.cpp` service discovery + a matching
   800×480 touch surface — the phone won't START the stream on a mismatch).
4. **Audio delivery is bursty** (dongle WiFi: measured most gaps 161–600 ms, in runs).
   Refilled `g_aaRing` several times per UI loop; `JitterBuffer` got a 480 ms prebuffer
   + a small **quick-resume** threshold (resume after a transient gap without a full
   re-prime — the re-prime was adding a prebuffer-long silence on top of each gap).

Result: `under` ≈ 1 brief blip / 4 s, `primes=1` (never collapses), `dropped=0`,
real-time decode, video smooth. Residual = the dongle's wireless burstiness (only HU
lever left is more buffer = more latency). Detail in `AA_AUDIO_INVESTIGATION.md`.

**Still open (see `HARDWARE_TODO.md` H8):** the intermittent xHCI `Transfer error 4`
on the bulk-IN during the TLS handshake. Its board hard-hang was carsh's own ISR
assert, fixed 2026-09-25: the error now ends the session and the dongle is
re-cycled/re-enumerated automatically (`AaLinkSupervisor`, bench verification
pending). The trigger itself is still unexplained. Also video frame-drop-when-behind
(no policy yet).

---

## Review-fix branch — 2026-09-25 (`claude/review-fixes`; host + device build only)

A full code review, then fixes. All host-tested (449 host tests) and cross-built in
all six device variants; **nothing here has run on the Pi** — the bench checklist is
`HARDWARE_TODO.md` H9. AA-relevant results:
- **AA touch and media keys had never reached the phone.** `sendTouch` put
  `touch_event` in field 2 (`disp_channel`, so the phone skipped it) with action and
  action_index swapped, and media keys went out as field 3 (= `touch_event`). The
  "hardware-validated `sendTouch`" in STATUS was never backed by a bench log. Now the
  aasdk `InputEventIndication` layout (touch_event 3, button_event 4), golden-byte
  tested, plus raw PRESS/DRAG/RELEASE streaming (rung 7 above).
- **The H8 hang** was `GetResultLength()` on the failed bulk-IN URB — an assert inside
  the xHCI ISR. Link faults now latch the transport dead (`AaBulkLink`) and
  `AaLinkSupervisor` ends the session, gives the dongle 3 s to re-cycle, then forces a
  re-enumeration (max 3). A phone Exit forces nothing.
- **Session lifecycle:** SHUTDOWN_REQUEST acknowledged; a fatal TLS error (lost or
  corrupted record) → Shutdown(CryptoFailure) instead of a silent wedge;
  `AaProjection::disconnect()` resets the AA tab. The mic channel is answered (no
  uplink), PingResponse echoes the int64 timestamp, reassembly is capped at 1 MiB, and
  the OTA reboot no longer destroys the decoder under the decode core.
- **Now Playing:** AA stays the session owner while paused, so '>' resumes it.
