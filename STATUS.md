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
| Host backend: PngRenderTarget | todo | UI phase. |
| Decoders (WAV/FLAC/MP3) | todo | |
| Decoders (AAC/ALAC/Opus) | todo | |
| AudioEngine + DSP chain host | todo | |
| DSP/EQ (biquads, RG) | todo | |
| Library DB + indexer + search | todo | |
| Tag parsers | todo | |
| Cover-art + ASCII generator | todo | |
| UI toolkit + 7 screens | todo | |
| ProfileManager | todo | |
| EQ presets | todo | |
| OTA package/crypto/A-B logic | todo | |
| Bluetooth (A2DP/AVRCP) | stubbed (planned) | Logic stubs only; needs controller. |
| Boot/CKernel, Circle backends | stubbed (planned) | HW bring-up; not host-testable. |
