# Investigation: choppy Android Auto audio

> **RESOLVED on the bench 2026-06-29 — "good enough":** fixed by a chain of changes,
> each bench-confirmed via the serial `aa-audio`/`aa-decode` instrumentation:
> 1. **Decode on a dedicated A72 core** (`device/CircleAaDecodeCore.h` via
>    `CMultiCoreSupport`; enabled `ARM_ALLOW_MULTI_CORE`, rebuilt libcircle). Frames
>    cross to the UI core through a lock-free triple buffer (`src/aa/FrameRelay.h`).
>    This took the ~37 ms/frame decode off core 0 — necessary but not sufficient.
> 2. **Refill `g_aaRing` several times per UI loop** (not once per ~74 ms render).
> 3. **Request 480p (800×480, the panel's native size) instead of 720p.** 720p
>    software decode was *slower than real-time* (~37 ms > 33 ms) → AU queue backed
>    up → dropped keyframes → **screen froze** until the next IDR. 480p decodes in
>    ~15 ms (real-time, `dropped=0`) so the freeze is gone, with less memory-bus
>    contention.
> 4. **Jitter buffer: quick-resume + 480 ms prebuffer.** The residual stutter is the
>    **dongle delivering audio in bursts** over its WiFi link (measured: most gaps
>    161–600 ms, in runs) — not an HU bug. A big cushion + resuming after a transient
>    gap without a full re-prime makes it smooth-enough.
>
> Final bench run: `under` ≈ 1 per 4 s as brief blips (`primes=1` — never collapses
> into a re-prime), `dropped=0` (no freeze), `dec` ~15–29 ms (real-time). Residual
> imperfection = dongle WiFi burstiness; the only further HU lever is more buffer =
> more latency. Branch: `claude/aa-decode-core`.

> **Update 2026-06-28 (host work, NOT yet bench-validated):** the mechanism was
> confirmed by code-reading (see "Root cause" below), instrumentation was added
> (steps 1–3), and the two low-risk fixes — a **prebuffer/jitter buffer on
> `g_aaRing`** and **decoupling H.264 decode from the audio-refill loop via an AU
> queue** — are implemented, host-compiled, and host-tested (`JitterBuffer`,
> `AuQueue`, `AsyncH264Decoder` suites; 181 tests green). The dedicated-A72-core
> decode (candidate fix #1) is provided as a **compiling scaffold**
> (`device/CircleAaDecodeCore.h`), deliberately not wired. **None of this is proven
> to fix the choppiness** — the symptom only reproduces on hardware and there has
> been no bench listen yet. See "What changed" + "Bench validation" at the bottom.

**Status:** open. AA video + audio both work end-to-end on the real Pi 4 (CA525 dongle
+ Galaxy S23 Ultra over USB), validated on the bench 2026-06-28. Video projects and
auto-displays; AA media audio plays out the 3.5 mm (PWM) jack. **But the audio is
choppy** (gaps/stutter), and the video is "slightly laggy". This doc scopes the
choppy-audio investigation for the next agent.

This is a **host-reasoned plan**: the actual symptom only reproduces on hardware (the
`tools/aa_linux` PC harness is headless and has no audio sink). Every fix must be
validated on the bench — see "Validation" below. Do NOT claim it fixed without a
bench listen.

## How AA audio flows today

```
phone --USB bulk-IN--> CAndroidAutoDevice (IRQ -> SPSC byte ring)
   --> CircleAaDriver::tick() [UI-loop thread, ~30 fps]
         -> AaConnection::poll() -> per-frame decrypt + reassemble
         -> AaProjection::onAvMedia(ch=4 MediaAudio)
         -> kernel.cpp playAudio hook: 16-bit LE PCM -> float -> g_aaRing.write()
PWM DMA/IRQ [independent, steady 48 kHz] -> BtMixSink::render()
         -> if g_aaRing.readable() >= want: read + scale by g_masterGain, else fall through
```

Key facts:
- AA opens exactly one audio channel: **MediaAudio (ch=4), 48 kHz stereo 16-bit PCM**
  (`src/aa/AaConnection.cpp` service discovery). Frames seen on the bench: ~8200 bytes
  (~42 ms) each, encrypted. No speech/system audio channel is advertised.
- The **producer** (`g_aaRing.write`) is driven by the **UI-loop thread** inside
  `CircleAaDriver::tick()` — the *same thread that runs the software H.264 video
  decode* (`CircleH264Decoder::feed()` → openh264, synchronous, one A72 core).
- The **consumer** is the PWM sound device's DMA/IRQ callback pulling
  `BtMixSink::render()` at a steady 48 kHz (`device/CirclePwmSoundDevice.h`).
- `g_aaRing` is `PcmRing<float>(kRate * kChannels)` ≈ 1 s of stereo headroom.

## Prime hypothesis (rank 1): video decode starves the audio refill

The software H.264 decode is heavy and runs **synchronously in the UI-loop thread**,
the same thread that refills `g_aaRing`. When a frame (esp. a large keyframe) takes a
long time to decode, `tick()` is delayed, AA audio frames aren't drained from the USB
ring into `g_aaRing` promptly, and the steady-rate PWM consumer **underruns** →
`render()` sees `readable() < want` → it abandons AA and outputs the (silent) engine
→ audible gap. The "slightly laggy video" is the same root cause (decode can't keep
up on one core), so both symptoms likely share the fix.

This is the explicitly-anticipated next step in `scripts/fetch_openh264.sh`:
*"Run feed() on a dedicated A72 core (CMultiCoreSupport)."*

## Other hypotheses (investigate in order)

2. **No jitter buffer / bad underrun handling.** `BtMixSink::render()` (device/
   kernel.cpp) plays AA only when `readable() >= want`; on a short read it falls
   through to A2DP/engine, emitting a full frame of silence (a click/gap) instead of
   gracefully padding or holding. Even small producer jitter then becomes audible.
   There is no prebuffer threshold before playback starts.
3. **Producer cadence vs. consumer.** AA frames arrive ~every 42 ms but `tick()` runs
   ~every 33 ms and drains up to 256 msgs/tick, interleaved with video frames. Audio
   may be delivered in bursts (several frames, then none), so the ring sawtooths and
   dips below `want` near the troughs.
4. **PWM device open/close churn.** `wantOpen` is driven by `g_aaRing.readable() > 0`
   (device/kernel.cpp). If the ring momentarily empties, confirm the device does NOT
   close/reopen (it should only reopen on a rate change) — a reopen mid-stream would
   gap the audio. Check the open/close logic + `audio: PWM jack @ ...` log frequency.
5. **Clock drift.** PWM fixed at 48 kHz vs. the phone's true sample clock. A small
   mismatch causes a slow drift and periodic over/underrun. Likely a minor contributor
   vs. #1, but quantify if #1–#4 don't fully explain it.
6. **PWM output quality.** The 3.5 mm output is PWM, not a real DAC. Distinguish
   *choppy* (gaps/dropouts → buffering) from *noisy/gritty* (PWM quantization). This
   doc targets choppy; if it's actually grit, that's a separate DAC-quality track.

## Investigation steps

1. **Confirm underrun is the mechanism.** Instrument `BtMixSink::render()` and
   `g_aaRing`: count how often AA is active but `readable() < want` (underruns/sec),
   and log ring occupancy (`readable()`) at ~1 Hz. Expect underruns to line up with
   the choppiness. (Log from the consumer carefully — it's a DMA/IRQ context; prefer
   incrementing counters and printing them from the UI thread, like the bulk-IN error
   path in `CircleAaDriver`.)
2. **Measure decode + tick time.** Time `CircleH264Decoder::feed()` per call and the
   whole `CircleAaDriver::tick()`; log max/avg per second. Correlate long ticks/decodes
   with the underrun spikes from step 1. This confirms/denies the prime hypothesis.
3. **A/B test the hypothesis cheaply:** temporarily skip the actual decode (still drain
   the video frames, just don't call openh264) and listen — if audio goes smooth, #1
   is confirmed and the fix is to get decode off the audio thread.
4. **Check device open/close churn** (hypothesis 4): log every PWM open/close.

## Candidate fixes (likely in this order)

- **Move H.264 decode to a dedicated core** via `CMultiCoreSupport` (there's already a
  `device/CircleDecodeTask.h` scaffold + `CircleBtRunLoopTask.h` as a pattern). Feed
  encoded AUs to the decode core; the UI thread keeps refilling audio. Expected to fix
  *both* choppy audio and laggy video. Biggest, highest-value change.
- **Add a jitter/prebuffer to `g_aaRing`**: don't start AA playback until ~100–200 ms
  is buffered; on underrun, output silence (or hold/repeat) *without* switching the
  source away from AA, so a brief dip doesn't cause a hard gap. Fix the
  `readable() < want` fallthrough in `render()` to pad the partial instead of dropping
  to the engine.
- **Prioritise audio draining** in `tick()`/`poll()` so AA audio frames reach
  `g_aaRing` before video decode runs in that tick (decouple the two pipelines).
- If clock drift is real, add light resampling / drop-or-dup a sample on sustained
  drift, or drive the producer/consumer from a shared clock.

## Root cause (confirmed by reading; mechanism, not yet bench-measured)

Circle runs a **cooperative scheduler on a single core (core 0)**. `CircleDecodeTask`,
`CircleBtRunLoopTask`, and the UI loop are all cooperative tasks there; they only
switch at `Yield()`/`MsSleep()`. The PWM output is the exception — its DMA completion
runs in **IRQ context**, which *does* preempt, so the consumer keeps pulling
`g_aaRing` at a steady 48 kHz no matter what core 0 is doing.

Two things then combine to starve the producer:

1. **Decode ran inline on the refill loop.** `CircleAaDriver::tick()` →
   `AaConnection::poll()` (up to 256×/tick) → `AaProjection::onAvMedia`. For a video
   frame that called `CircleH264Decoder::feed()` (openh264) **synchronously, in the
   same loop iteration that writes AA audio into `g_aaRing`**. openh264 doesn't yield,
   so a long keyframe decode blocks core 0 — and thus the audio refill — for its whole
   duration while the IRQ keeps draining the ring.
2. **No prebuffer + hard fallthrough.** Playback started at `readable() > 0`, so the
   ring hovered near-empty (producer and consumer both run at real-time rate). Any
   stall from (1) instantly underran, and `BtMixSink::render` then abandoned AA for
   the (silent) local engine on the short read — turning each dip into an audible
   click rather than a graceful silent pad.

The 1 s ring capacity never helped because nothing kept it *filled*. The "slightly
laggy video" is the same single-core-decode bottleneck, which is why the dedicated
core is expected to help both.

## What changed (host-tested; bench-validation still required)

New, platform-independent + host-tested:
- `src/audio/JitterBuffer.h` — consumer-side prebuffer for `g_aaRing`: waits until
  `kAaPrimeMs` (≈120 ms, tunable in `kernel.cpp`) is buffered before playing, emits
  **silence on a short read while keeping AA as the active source** (kills the
  source-switch click), re-primes before resuming, and disengages + discards the tail
  when the stream stops. Counters (`underruns`, `primeEvents`, occupancy) drive the
  logs. Tests: `tests/audio/JitterBuffer_test.cpp`.
- `src/aa/AuQueue.h` — framed lock-free SPSC byte ring for whole H.264 access units
  (single-publish framing so the consumer never sees a half-written AU).
- `src/aa/AsyncH264Decoder.h` — an `IH264Decoder` whose `feed()` just **enqueues** the
  AU (cheap) and whose `pump()` runs the real decoder elsewhere; carries the A/B
  "skip decode" toggle (step 3). Tests: `tests/aa/AuQueue_test.cpp`.

Wired into the device image (compiles with the cross toolchain; **not** runnable on
the host — no audio sink / no Circle):
- `device/kernel.cpp` — `BtMixSink::render` now drives AA through `g_aaJitter`; the
  projection feeds `AsyncH264Decoder` instead of the decoder directly; a new
  cooperative `CircleAaDecodeTask` pumps openh264 off the UI loop; per-second
  serial health logs (`aa-audio:` ring/underruns/prime/tick-µs, `aa-decode:`
  decode-µs/queue/drops) + on-screen `au=` underrun counter. The first-frame
  auto-show was moved to the UI task (it sends `VIDEO_FOCUS` over the transport,
  which must stay on the UI thread now that decode is off it).
- `device/CircleAaDecodeTask.h` — the cooperative decode pump + `AaDecodeStats`.
- `device/CircleAaDecodeCore.h` — **scaffold only**, the `CMultiCoreSupport`
  dedicated-core version; not constructed. Its header lists the bench bring-up steps
  (double-buffer the frame, cache maintenance, atomic cross-core flags) that MUST be
  done before wiring it.

Expected effect (hypothesis, unproven): even on one core, the prebuffer should absorb
a single-AU decode stall and the decoupling stops decode sitting *inside* the audio
write, so underruns should drop sharply. The dedicated core (scaffold) is the further
step for sustained throughput / the laggy video.

## Validation (bench-only)

The fast loop (see `CLAUDE.md` → "The bench loop"): build
`cd device && make CARSH_HAVE_OPENH264=1`, flash `kernel8-rpi4.img` to the SD card,
boot the Pi with the CA525 + phone, re-plug the dongle if AA doesn't connect within
~15 s (it wedges across a Pi power-cycle), project, and **listen**. Watch the serial
log (`/dev/ttyACM0`, 115200) for the new audio instrumentation + the existing
`alive:` health line (since 2026-09-25: `alive: state=... tx=... rx=... inFlight=2
ring=0 link=...`; the `inErr`/`outErr` counters are gone — a transfer error now
latches the link dead, see `HARDWARE_TODO.md` H8). Success = smooth audio
through music + nav voice for minutes. Note in the log any underrun counter going to
zero.

Heads-up from this session: the **64 KB bulk-IN buffers** fix (in
`device/CircleAoaTransport.h`) is what made the USB transport stable — do not shrink
them. And the dongle wedges across Pi reboots; re-plug it to recover.

### Bench checklist for the new prebuffer + decouple work

1. **Confirm the mechanism (step 2/3) before trusting the fix.** Read the new logs:
   - `aa-audio: ring=… prime=… eng=… under=… primes=… tick(us) max=… avg=…`
   - `aa-decode: n=… dec(us) max=… avg=… q=… pushed=… dropped=…`
   With music playing, watch whether `under=` climbs in lockstep with `dec(us) max`
   spikes / `q=` (queue) backing up. That correlation IS the prime hypothesis.
2. **A/B the decode (cheap, decisive).** Build with decode skipped:
   `make clean && CPPFLAGS=-DCARSH_AA_SKIP_DECODE make CARSH_HAVE_OPENH264=1` and
   listen. (Pass it through the environment: `CPPFLAGS=` on the make command line
   replaces every C++ flag the Makefile adds, and `make clean` because the Makefile
   doesn't track the flags.) AUs still
   drain (no queue backup) but openh264 never runs. If audio goes smooth, decode load
   is confirmed as the cause — and the dedicated-core scaffold becomes the priority.
3. **Then test the real fix build** (`make CARSH_HAVE_OPENH264=1`, decode on): success
   = `under=` stays at/near 0 and `au=` on screen stops climbing through minutes of
   music + nav voice. If underruns drop but aren't zero, raise `kAaPrimeMs` in
   `kernel.cpp` (more latency, more slack); if audio noticeably lags the video, lower
   it. If `dropped=` on the AU queue climbs, decode can't keep up on one core → wire
   the `CircleAaDecodeCore` scaffold (do its header checklist first).
4. **Watch for regressions** the host can't see: video tearing/lag from the moved
   decode, the auto-show still firing once, and the `alive:`/`inFlight` transport
   health staying as before (the 64 KB buffers + this change are independent).

**Do not mark this fixed without completing 3.** Host tests only prove the buffer/queue
logic, not that the Pi's audio is smooth.

## Key files

| File | Role |
|---|---|
| `device/kernel.cpp` | `BtMixSink::render` (output mix + gain), `playAudio` hook (PCM→`g_aaRing`), `wantOpen`/PWM open, `g_aaRing`, `g_masterGain` |
| `device/CircleH264Decoder.cpp` | software openh264 decode (the suspected CPU hog), synchronous |
| `device/CircleAaDriver.cpp` | `tick()` — drives `poll()` on the UI thread (producer side) |
| `src/aa/AaProjection.h` | routes `onAvMedia` → decoder (video) / `playAudio` (audio) |
| `device/CirclePwmSoundDevice.h` | PWM jack output (the steady-rate consumer) |
| `src/audio/PcmRing.h` | the SPSC ring (`g_aaRing`) |
| `src/audio/JitterBuffer.h` | **new** — prebuffer/underrun handler over `g_aaRing` (fix #2) |
| `src/aa/AuQueue.h`, `src/aa/AsyncH264Decoder.h` | **new** — AU queue + async decoder (fix #3, decouple) |
| `device/CircleAaDecodeTask.h` | **new** — cooperative decode pump + `AaDecodeStats` (timing) |
| `device/CircleAaDecodeCore.h` | **new scaffold** — dedicated-A72-core decode (`CMultiCoreSupport`), not wired |
| `device/CircleDecodeTask.h`, `CircleBtRunLoopTask.h` | existing cooperative-task patterns to model the decode pump on |

## Context / prior art

- `STATUS.md` — module states. `AA_PLAN.md` — the AA bring-up log. This session's wins:
  video + audio end-to-end on HW, the 64 KB buffer fix for the xHCI stall, AA audio
  output + HU volume.
- The functional reference is `dylant9/NexusDrive` (Linux AA head unit) — useful for
  how it buffers/clocks AA audio if the jitter-buffer route is taken.
