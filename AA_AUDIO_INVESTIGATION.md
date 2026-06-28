# Investigation: choppy Android Auto audio

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

## Validation (bench-only)

The fast loop (see `CLAUDE.md` → "The bench loop"): build
`cd device && make CARSH_HAVE_OPENH264=1`, flash `kernel8-rpi4.img` to the SD card,
boot the Pi with the CA525 + phone, re-plug the dongle if AA doesn't connect within
~15 s (it wedges across a Pi power-cycle), project, and **listen**. Watch the serial
log (`/dev/ttyACM0`, 115200) for the new audio instrumentation + the existing
`alive: ... inFlight=2 ring=0 inErr=0 outErr=0` health line. Success = smooth audio
through music + nav voice for minutes. Note in the log any underrun counter going to
zero.

Heads-up from this session: the **64 KB bulk-IN buffers** fix (in
`device/CircleAoaTransport.h`) is what made the USB transport stable — do not shrink
them. And the dongle wedges across Pi reboots; re-plug it to recover.

## Key files

| File | Role |
|---|---|
| `device/kernel.cpp` | `BtMixSink::render` (output mix + gain), `playAudio` hook (PCM→`g_aaRing`), `wantOpen`/PWM open, `g_aaRing`, `g_masterGain` |
| `device/CircleH264Decoder.cpp` | software openh264 decode (the suspected CPU hog), synchronous |
| `device/CircleAaDriver.cpp` | `tick()` — drives `poll()` on the UI thread (producer side) |
| `src/aa/AaProjection.h` | routes `onAvMedia` → decoder (video) / `playAudio` (audio) |
| `device/CirclePwmSoundDevice.h` | PWM jack output (the steady-rate consumer) |
| `src/audio/PcmRing.h` | the SPSC ring (`g_aaRing`) |
| `device/CircleDecodeTask.h`, `CircleBtRunLoopTask.h` | existing multi-core/task scaffolds to model a decode core on |

## Context / prior art

- `STATUS.md` — module states. `AA_PLAN.md` — the AA bring-up log. This session's wins:
  video + audio end-to-end on HW, the 64 KB buffer fix for the xHCI stall, AA audio
  output + HU volume.
- The functional reference is `dylant9/NexusDrive` (Linux AA head unit) — useful for
  how it buffers/clocks AA audio if the jitter-buffer route is taken.
