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
| AA H.264 decode (`CircleH264Decoder`+`Yuv420`) | host-done / `[hw]` FPS | openh264-backed `IH264Decoder`; I420→RGB888 (BT.601) pure converter. Host build links **system openh264 2.4.1** (same line vendored for the Pi) and runs a real encode→decode→pixel-compare round trip (`H264Decode_test`, MAE-checked) + converter anchors. Device links the cross-built decoder via `make CARSH_HAVE_OPENH264=1` (scaffold: `scripts/fetch_openh264.sh`). `[hw]`: aarch64 bare-metal openh264 build, on-target FPS, dedicated-core run. |
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
| Library: Database | host-done | Upsert, token search (AND+prefix), browse, persist. |
| Library: SmartPlaylist | host-done | Rule engine; All/Any, sort, limit. |
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
| AA connection + channels (`AaConnection`) | host-done | Bring-up state machine (version→TLS→auth→service discovery→channel opens) + rung-4c per-channel dispatch: AV setup/start/stop/media (+ `AV_MEDIA_ACK` flow control), input binding + outbound touch, sensor start + driving-status/night event, video focus. 7 AA tests (`AaConnection`/`AaChannels`); wire acceptance by a real phone is `[hw]`. |
| Android Auto UI (`AndroidAutoScreen`+`AaVideoGeometry`) | host-done | 9th "AA" tab: aspect-fit video blit into the content rect + touch→source(800×480) remap + tab-focus callbacks (VIDEO_FOCUS). 11 host tests; links into the Pi image (placeholder until the transport feeds it). |
| AA TLS crypto (`AaCryptoMbedTls`) | host-done (plumbing) / `[hw]` live handshake | mbedTLS-backed `IAaCrypto` (HU = TLS 1.2 client, ECDHE-RSA-AES128-GCM, VERIFY_NONE, in-memory BIO). Compiled into the host build vs **system mbedTLS 2.28** (same line circle bundles): a real in-memory client↔server handshake + record round-trip pass (`AaCryptoMbedTls_test`, incl. the raw-cert bypass path). Links into the image via the mbedTLS archives. `[hw]`: the live phone handshake + swapping the placeholder cert for the Google-registered aasdk cert (`AaHuCert.h`). |
| AA USB-AOA transport (`CircleAoaTransport`) | hw bring-up (rungs 1-2 done) | On `device/aa-dongle-bringup`. CA525 enumerates as `ven2717-ff40` (Android PTP); factory hook drives the AOA mode-switch → dongle re-enumerates in accessory mode `ven18d1-2d00` (confirmed on hardware). Accessory bulk-endpoint binding blocked by a Circle hub port-flag bug; fix = `usbstandardhub-reconfigure.patch`, **pending bench re-test**. Async bulk-IN→ring + bulk-OUT mirror the BT ACL pattern. |

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
