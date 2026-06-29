# carsh — Module Status

Auditable per-module state. Legend:
- **hw-validated** — confirmed working on the real Pi 4 (not just built).
- **host-done** — compiles on the build server AND its host unit tests pass.
- **stubbed** — logic exists behind its interface; needs hardware to validate.
- **blocked** — cannot proceed without a hardware fact or a decision (see backlogs).
- **todo** — not started.

Host method: every subsystem is built against the HAL interfaces (`ISoundDevice`,
`IRenderTarget`, `Vfs/Source`, `ITouch`) with host backends (WAV sink, PNG target,
host FS, synthetic touch). "host-done" never means "validated on the Pi" — see
HARDWARE_TODO.md for what the bench session must confirm.

> Build note: the AArch64 cross-toolchain (`/opt/arm-toolchain`, aarch64-none-elf
> 14.2) + circle-stdlib are installed and build `kernel8-rpi4.img`, which now boots
> on a real Pi 4 (see the device-build section). The host table below is g++
> host-compiled/host-tested; "host-done" still never means "validated on the Pi".

| Module | Status | Notes |
|---|---|---|
| Test harness (`test/`) | host-done | Assertion-based runner; `CHECK/REQUIRE/CHECK_NEAR`. |
| PcmRing (SPSC) | host-done | Lock-free, wrap, discard, concurrent SPSC test. |
| HAL interfaces | host-done | ISoundDevice/IAudioSink, IRenderTarget, Vfs/IStream/Source, ITouch. |
| Host backend: WavSinkDevice | host-done | Pull-pump model; captures float→WAV; underrun count. |
| Host backend: WavFile r/w | host-done | 16/24/32-bit int + float32 round-trips. |
| Host backend: HostVfs/HostSource | host-done | Real-dir Vfs; tested indirectly via decoders/indexer. |
| Host backend: SyntheticTouch | host-done | Scripted tap/swipe event injector. |
| Host backend: MemStream | host-done | In-RAM IStream for fixtures/OTA blobs. |
| Host backend: PngRenderTarget | host-done | ARGB surface -> PNG on present(). |
| UI: Canvas + 8x8 font | host-done | Primitives, glyph blit, text, alpha blend. |
| UI: Theme | host-done | Dark/Light/Amber; accent #d97757. |
| UI: GestureRecognizer | host-done | Tap vs vertical swipe from touch stream. |
| UI: Widgets | host-done | Button, Label, ListView (select+scroll), TabBar. |
| UI: ScreenManager + tabs | host-done | Tab routing, gesture dispatch, render. |
| UI: 7 screens | host-done | Now/Lib/Find/Queue/EQ/Src/Set; PNG + touch tested. |
| UI: LibraryScreen | host-done | "Lib" tab: hierarchical browse (header + list + BACK/PLAY/SHUFFLE), scope-aware captions; drives `LibraryBrowser`. 4 host tests (`LibraryScreen_test`). |
| AA H.264 decode (`CircleH264Decoder`+`Yuv420`) | host-done / `[hw]` FPS | openh264-backed `IH264Decoder`; I420→RGB888 (BT.601) pure converter. Host build links **system openh264 2.4.1** (same line vendored for the Pi) and runs a real encode→decode→pixel-compare round trip (`H264Decode_test`, MAE-checked) + converter anchors. Device links the cross-built decoder via `make CARSH_HAVE_OPENH264=1`. **The aarch64 bare-metal decoder lib now builds + links into the image** (`scripts/build_openh264_baremetal.sh`: decoder+common+NEON at Circle's ABI, newlib pthread/sem shim headers in `scripts/openh264_baremetal_include/` + a no-op threading shim `scripts/openh264_baremetal_shim.c`; single-threaded `DecodeFrameNoDelay`, `HAVE_NEON_AARCH64` constant CPU-detect). **2026-06-29 bench: decodes on the board, real-time at 480p (~15 ms/frame; 720p was ~37 ms > the 33 ms budget → queue overflow → dropped keyframes → screen freeze, so we request 480p = native 800×480).** Runs on a dedicated A72 core — see the decode-core row below. |
| AA decode core + frame relay (`CircleAaDecodeCore`, `FrameRelay`) | hw-validated (2026-06-29, PR #24) | H.264 decode runs on a **dedicated A72 core** (`CMultiCoreSupport`, core 3) so it can't starve the core-0 audio refill — the bench-confirmed fix for choppy AA audio. Needs `ARM_ALLOW_MULTI_CORE` (`scripts/patches/sysconfig-multicore.patch`; rebuilt libcircle, inner-shareable cacheable memory). Decoded frames cross core 3→0 via a lock-free triple buffer (`src/aa/FrameRelay.h`, host-tested incl. a 20k-frame threaded no-tearing stress test); AU input via `src/aa/AuQueue.h`. Gated `CARSH_AA_DECODE_CORE` (default on with openh264; `CARSH_NO_DECODE_CORE=1` = single-core cooperative task fallback). Audio smoothed further by multi-refill-per-UI-loop + `JitterBuffer` 480 ms prebuffer + quick-resume (bursty dongle WiFi). |
| Decoders (WAV/FLAC/MP3) | host-done | dr_libs; WAV bit-exact, FLAC lossless, MP3 tone. |
| Decoder: Opus | host-done | libopusfile; real .opus fixture, tone-correlated. |
| Decoder: AAC (ADTS) | host-done | faad2; real .aac fixture, tone-correlated. |
| Decoder: AAC-in-MP4 / ALAC | deferred (host-buildable) | Needs MP4 demux (+Apple ALAC, +encoder for fixtures). See "Deferred host work". |
| AudioEngine + DSP chain host | host-done | RG→EQ hook→vol→limiter; underrun/rendered counts; pause. |
| DecodePump (decode task) | host-done | Block decode + gapless handoff + track boundaries. |
| PlayerController | host-done | Transport FSM, queue, gapless, seek, pause, position. |
| DSP: Biquad (RBJ) | host-done | Peak/shelf/LP/HP; verified vs analytic |H|. |
| DSP: EqProcessor | host-done | Cascade + preamp; RT-safe double-buffer swap; ISampleProcessor. |
| DSP: ReplayGain apply | host-done | dB<->lin, R128 ref gain, peak-clamped resolve. |
| DSP: loudness measure (BS.1770) | host-done | K-weighting + gating; rate-correct coeffs; verified. |
| Library: TextNorm | host-done | UTF-8 casefold + accent strip + tokenize. |
| Library: Database | host-done | Upsert, token search (AND+prefix), browse, persist. `byArtist`/`albumsByArtist` browse helpers (normalized album-artist match). |
| Library: SmartPlaylist | host-done | Rule engine; All/Any, sort, limit. |
| Library: LibraryBrowser | host-done | Hierarchical Artists→Albums→Songs nav + play/shuffle scoped to the current level (whole library / artist / album), seedable Fisher-Yates shuffle. 8 host tests (`LibraryBrowser_test`). |
| Tag parsers (ID3v2/Vorbis/FLAC/Opus) | host-done | Real-encoder fixtures; lame UTF-16 quirk handled. |
| Tag parsers (MP4/M4A) | deferred | Needs MP4 demux (with AAC-MP4/ALAC). |
| Library: Indexer | host-done | Interruptible scan, tags+props, RG track+album scan. |
| Cover-art + ASCII generator | host-done | stb_image decode, downscale, ramp, tint, cache blob. |
| ProfileManager | host-done | MAC→profile, guest clone+remember, persist. |
| Profile / EQ preset format | host-done | Binary serialize round-trips. |
| OTA: Crypto (ICrypto) | host-done | OpenSSL host backend (real Ed25519+SHA-256). |
| OTA: Manifest/Package | host-done | Flat-JSON manifest, .carup verify (sig+image hash). |
| OTA: BootSlots A/B SM | host-done | decide/stage/confirm/rollback + record persist. |
| OTA: OtaManager apply | host-done | verify→version-gate→write inactive→arm trial. |
| OTA: flash write + reboot | stubbed — needs hardware | IFlashTarget host stub; real SD write = H7. |
| Bluetooth seam (IBluetooth) | host-done (stub) | Interfaces + FakeBluetooth; connect→profile, AVRCP→now-playing, A2DP→ring tested. |
| Bluetooth real A2DP/AVRCP | hw-validated (A2DP audio) | `CARSH_HAVE_BTSTACK=1` + UB500 dongle: a phone pairs and plays music out the 3.5 mm jack. Realtek firmware load, ACL path (CUSBBluetoothDevice patch), SBC→PWM all working on hardware. Pairing UX + AVRCP = polish (H6). |
| AVRCP now-playing assembly | host-done | `bt::NowPlayingAssembler` accumulates per-attribute subevents; 7 tests (BtParse). |

## Device build (Circle / bare metal) — booting on real hardware

The image **boots on a real Pi 4 and runs as a working media player**: it brings up
the DSI panel, takes touch input, mounts a USB media stick and indexes it, and plays
audio out the onboard 3.5 mm jack (PWM) — with a USB-C serial console for telemetry.
The STL-heavy core links unchanged into the bare-metal image with `aarch64-none-elf`
+ circle-stdlib (`STDLIB_SUPPORT=4`). See device/README.md.

| Module | Status | Notes |
|---|---|---|
| Toolchain + circle-stdlib | done | ARM aarch64-none-elf 14.2; circle-stdlib Step pinned; newlib+libstdc++ build. |
| `CKernel` (boot/serial/timer) | hw-validated | Boots on real Pi 4; serial log @115200; UI renders on the panel. |
| `CircleRenderTarget` (`IRenderTarget`) | hw-validated | `CBcmFrameBuffer`, double-buffered, pitch-aware; pixels confirmed on a DSI panel. |
| `CircleVfs`/`CircleSource` (`Vfs`) | hw-validated | FatFs scans a real USB FAT32 stick; albums/tracks populate the library (hidden dot-folders skipped). |
| `CirclePwmSoundDevice` (`ISoundDevice`) | hw-validated | Onboard PWM → 3.5 mm jack; audible playback confirmed. Lazy open at track rate; safe close; peak meter. |
| `CircleSoundDevice` (I2S DAC) | builds | `CI2SSoundBaseDevice` pull→float→DAC range. Awaiting a DAC HAT (H3). |
| `CircleTouch` (`ITouch`) | hw-validated | `CRPiTouchScreen` pumped >60 Hz; tap/swipe drive the UI on the panel (FingerUp(0,0) handled). |
| `CircleUsbLog` (USB-C serial console) | hw-validated | CDC-ACM gadget on the USB-C/dwc2 port; logger retargets to `/dev/ttyACM0` on connect, UART fallback. |
| UI: vertical volume rail | hw-validated | Left-edge slider, boots muted, logarithmic taper (−40 dB range), capped at 0.5 gain. |
| `CircleFlashTarget` (`IFlashTarget`) | builds | FatFs write of kernel8-A/B.img; rollback = bench (H7). |
| Full app integrated in `CKernel` | hw-validated | SD→library→player→EQ→profiles→UI+8 tabs+touch all running on the board. |
| Real-time decode/scheduler task | hw-validated | `CircleDecodeTask` under `CScheduler` feeds playback on hardware. Underrun tuning under load = H4. |
| OTA device crypto (`DeviceCrypto`) | builds + host-validated | TweetNaCl Ed25519 + SHA-256 (mbedTLS 2.28 has no Ed25519). Cross-checked vs OpenSSL. |
| OTA verify on device | builds | Kernel reads/verifies staged `/carsh/ota/*.carup` (no auto-flash; user-confirmed). |
| Bluetooth scaffold (`CircleBluetooth`) | builds (stub) | `bt::IBluetooth` impl + run-loop `CTask` + A2DP→ring/AVRCP→profile wiring + scan/connect screen; BTstack glue gated by `CARSH_HAVE_BTSTACK`. |
| BT scan/connect UI (`DevicesScreen`) | host-done | 8th "BT" tab: scan toggle + connect/disconnect; 11 tests via FakeBluetooth (`BtUi_test`). |
| BTstack A2DP/AVRCP glue | hw-validated (A2DP audio) | On `device/bt-dongle-bringup`: phone→Pi A2DP audio works. Fixed USB-IRQ deadlock, HCI event reassembly, Realtek firmware download (product-id + custom-pre-init), USB ACL path (CUSBBluetoothDevice patch), SBC media-header strip + PWM routing. |
| AA connection + channels (`AaConnection`) | hw-validated (through channel setup) | Bring-up state machine (version→TLS→auth→service discovery→channel opens) + rung-4c per-channel dispatch. **2026-06-28 bench (CA525 + S23 Ultra): the full sequence runs on the board** — version → live TLS → auth → service discovery → all 8 channels open → AV setup / sensor-start / input-binding all answered, with the phone accepting every response. Bugs fixed on hardware this session: (1) CHANNEL_OPEN_REQUEST arrives on the *target* channel (not Control) — route by msg id, reply on that channel; (2) channel-control responses (AV setup / sensor-start / input-binding) must be `MessageType::Control`, not Specific (Specific = phone treats SETUP_RESPONSE as stray media, never starts AV); (3) proactive `VIDEO_FOCUS_INDICATION(FOCUSED)` after Video setup; (4) AUDIO_FOCUS / NAV_FOCUS request handlers; (5) SD encoding (AVInputChannel stream_type/audio_config fields, VideoFPS `_30=1`). 8 AA tests. **UPDATE 2026-06-28 (PR #14, `tools/aa_linux` + real S23 over USB): AA video now streams end-to-end (690+ H.264 frames / 4 MB+, sustained).** This SUPERSEDES bug (2) above: only `CHANNEL_OPEN_RESPONSE` is `Control`; every other HU response (AV-setup/sensor-start/binding/focus) is `Specific` — sending them `Control` set channels up but never streamed. Other root causes fixed: `AVChannelMessageIdsEnum` was shifted (VIDEO_FOCUS_REQUEST=0x8007 / _INDICATION=0x8008; MEDIA_ACK=0x8004); aasdk decrypts each encrypted frame INDIVIDUALLY (per-frame decryptor, reassembly is frame-type-driven not size-driven); service discovery = exactly 5 channels with the Video channel carrying `stream_type=VIDEO(3)`; NO periodic sensor tick (reactive only). The device image links all of this (`src/aa`) + the cross-built openh264 decoder. **Remaining `[hw]`: on-board projection — gated by the transport reset below (the streaming fixes were proven on a PC USB-AOA path that bypasses Circle-xHCI).** |
| AA projection wiring (`AaProjection`) | host-done / `[hw]` live | Rung-6 coordinator: video media→decoder→`AndroidAutoScreen::setFrame`, audio media→PCM hook (ring), channel-open/shutdown→connected state, content-rect touch→`sendTouch`, AA-tab focus→`sendVideoFocus`. Host-tested end to end with fakes (`AaProjection_test`). Bound to the live connection by `CircleAaDriver`; real hooks + decoder in `kernel.cpp`. `[hw]`: on-board projection + AA audio out the jack (rung 7). |
| Android Auto UI (`AndroidAutoScreen`+`AaVideoGeometry`) | host-done | 9th "AA" tab: aspect-fit video blit into the content rect + touch→source(800×480) remap + tab-focus callbacks (VIDEO_FOCUS). 11 host tests; links into the Pi image (placeholder until the transport feeds it). |
| AA TLS crypto (`AaCryptoMbedTls`) | hw-validated (live handshake) | mbedTLS-backed `IAaCrypto` (HU = TLS 1.2 client, ECDHE-RSA-AES128-GCM, VERIFY_NONE, in-memory BIO). **2026-06-28 bench: the live TLS 1.2 handshake against a real phone completes on the board** using the real Google-registered aasdk / JVC-Kenwood cert in `AaHuCert.h` (the placeholder is gone). Two bugs fixed on hardware: (1) the cert swap; (2) **circle-mbedtls is built `MBEDTLS_THREADING_C`+`THREADING_ALT`, so the mutex callbacks must be installed via `mbedtls_threading_set_alt` before any context — carsh used raw mbedTLS and never did, so the first lock returned `-0x001c` (`THREADING_BAD_INPUT_DATA`) and the ClientHello was never produced. `CircleAaDriver` now constructs a `CircleMbedTLS::CThreadingModule` once at bind.** Host round-trip still passes vs system mbedTLS 2.28. |
| AA USB-AOA transport (`CircleAoaTransport`) | hw bring-up (rungs 1-4b done) / **blocked on bulk reset** | CA525 enumerates `ven2717-ff40` (PTP) → AOA switch → accessory `ven18d1-2d00` → bulk endpoints bound as `uaa1` → drives version/TLS/discovery/channel-setup live (above). Double-buffered async bulk-IN→ring; blocking bulk-OUT. **Open blocker (risk #2, the bulk nemesis):** ~0.1-3 s into the Running phase, during the phone's channel-open/AV-setup burst, the bulk-IN throws xHCI completion-code-4 (`USBErrorUnknown`) and the dongle hard-resets to PTP (phone shows "communication error 2"). The failure point is **non-deterministic** (varies channel-opens → AV-setup → idle across runs), so it's a timing-sensitive USB/xHCI issue, not protocol. Tried + ruled out: async bulk-OUT (regressed, reverted), double-buffered bulk-IN (no change, kept), IN endpoint recovery (dongle truly re-enumerates). Dongle proven healthy on a Linux PC (AA link mode, paired S23 Ultra, 720p). Next: OpenAuto/usbmon on the PC to confirm Circle-vs-dongle + a reference capture, or a Circle-xHCI deep-dive. |

> The official 7" panel that wouldn't light up turned out to be a loose DSI ribbon,
> not firmware — re-seating fixed it. Display bring-up was confirmed on the spare
> ESP32-P4 dev-kit DSI panel. The `scripts/make_sdcard.fish` helper assembles a
> correct FAT32 boot partition in one step.

## Deferred host-buildable work (not hardware-gated, not yet done)

These are buildable/testable on the host but deferred to keep breadth across all
subsystems. Not blocked — just remaining integration:

- **AAC-in-MP4 (.m4a) + ALAC.** ADTS AAC is done via faad2. `.m4a`/ALAC need a
  small MP4 (ISO-BMFF) demuxer to extract the elementary stream, then faad2 for
  AAC or the Apple ALAC reference for ALAC. Also needs an MP4/ALAC encoder on the
  build server for fixtures (ffmpeg, currently absent). The sniffer already
  recognizes `ftyp` and routes to the AAC codec id; `createDecoder` returns null
  until the demux lands, so such files are catalogued but not yet decodable.
- **Sample-rate converter (speexdsp).** Needed for crossfade across mismatched
  rates and BT reclock. The engine + controller currently run one rate; the
  reclock/SRC path is stubbed (gapless preload is declined on rate mismatch).
