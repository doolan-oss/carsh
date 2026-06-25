# carsh — Module Status

Auditable per-module state. Legend:
- **host-done** — compiles on the build server AND its host unit tests pass.
- **stubbed** — logic exists behind its interface; needs hardware to validate.
- **blocked** — cannot proceed without a hardware fact or a decision (see backlogs).
- **todo** — not started.

Host method: every subsystem is built against the HAL interfaces (`ISoundDevice`,
`IRenderTarget`, `Vfs/Source`, `ITouch`) with host backends (WAV sink, PNG target,
host FS, synthetic touch). "host-done" never means "validated on the Pi" — see
HARDWARE_TODO.md for what the bench session must confirm.

> Build server note: the AArch64 cross-toolchain and `qemu-system-aarch64` named in
> the task brief are **not installed** in this environment (only host g++ 15 +
> cmake). All work below is host-compiled/host-tested with g++. See HARDWARE_TODO.md
> item H0.

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
| Host backend: PngRenderTarget | todo | UI phase. |
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
| UI toolkit + 7 screens | todo | |
| ProfileManager | todo | |
| EQ presets | todo | |
| OTA package/crypto/A-B logic | todo | |
| Bluetooth (A2DP/AVRCP) | stubbed (planned) | Logic stubs only; needs controller. |
| Boot/CKernel, Circle backends | stubbed (planned) | HW bring-up; not host-testable. |

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
