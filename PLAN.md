# carsh — Bare-Metal Automotive Head Unit: Technical Plan

A bare-metal (no Linux, no RTOS) media head unit for a Raspberry Pi 4 + official 7" DSI
touchscreen, built on the Circle C++ framework and BTstack. Terminal-aesthetic UI,
multi-source library, high-quality gapless audio, parametric EQ, driver profiles, and
A/B OTA.

This document is the build spec. It defines architecture, module boundaries, sequencing,
data formats, and risks. No application code yet.

---

## 0. Ground truth: what Circle gives us, and what bare-metal costs

Before architecture, anchor on reality, because it shapes every decision below.

**Circle (rsta2/circle)** is a mature bare-metal C++ framework for the Pi (0–5). On the
Pi 4 (AArch64) it provides, out of the box and proven:

- Boot/startup (`CKernel` pattern), MMU + caches, exception vectors, AArch64 EL2→EL1.
- Mailbox property interface (clocks, power, framebuffer, temperature, MAC address).
- DSI/HDMI framebuffer (`CBcmFrameBuffer`, `C2DGraphics`) — **the official 7" DSI panel
  is initialized by the VideoCore firmware before our code runs**, so we get a framebuffer
  without writing a DSI driver (see Risk R1 for the caveat).
- USB host stack (`CUSBHCIDevice`) — xHCI on the Pi 4. Supports USB mass storage,
  HID, hubs. This is the single biggest reason to use Circle.
- I2C (`CI2CMaster`) — needed for the touch controller and DAC control.
- I2S/PCM sound device (`CI2SSoundBaseDevice`) — clocks the I2S peripheral, DMA double-
  buffering, pull-model callback for sample data.
- FAT filesystem (`CFATFileSystem`) — FAT12/16/32. **No exFAT, no NTFS** out of the box
  (see R6). There is also an optional FatFs integration.
- Cooperative scheduler (`CScheduler`, `CTask`) — non-preemptive green threads with
  explicit `Yield()`. Plus timers, `CSpinLock`, event/synchronization primitives.
- Networking (`CNetSubSystem`) — TCP/IP stack, DHCP, DNS, TLS via a bundled mbed TLS
  in some samples; WiFi via the `WLAN`/`hostap`-derived firmware-loading driver for the
  onboard CYW43455.
- Bluetooth: Circle has a *vendor HCI transport* + firmware loader for the onboard
  controller. We **replace Circle's thin BT layer with BTstack** but reuse Circle's
  transport plumbing (see Bluetooth section).

**What bare-metal costs us** (these recur as risks):

- No `malloc` guarantees beyond Circle's heap; no virtual memory paging; no `mmap`.
  Big buffers must be planned and pre-allocated.
- No preemption. A decoder that runs 30 ms without `Yield()` glitches audio and freezes
  touch. Cooperative scheduling discipline is the dominant non-functional constraint.
- No POSIX. No threads-with-priority, no `pthread`, no `epoll`. Drivers either exist in
  Circle or we write them.
- No dynamic linking, no `dlopen`. Everything is statically linked into one image.
- Limited libc (Circle ships a partial newlib-like environment). Many ports need shims.

**Core consequence for the whole design:** the audio render path must be the highest-
priority cooperative citizen, must never be blocked by UI/indexing/BT, and must be fed by
a lock-free ring buffer filled by a decode task that yields frequently. Everything else is
secondary. Design top-down from "never underrun the I2S DMA."

---

## 1. System Architecture

### 1.1 Layer diagram

```
+--------------------------------------------------------------------------+
|  APPLICATION / UI LAYER                                                   |
|  Screens: NowPlaying  Library  Search  Queue  EQ  Sources  Settings      |
|  UI toolkit (custom): char-grid widgets, touch hit-testing, dirty regions|
+-------------------+------------------+-----------------+-----------------+
|  PlayerController  |  LibraryService  | ProfileManager  | OTAManager     |
|  (transport state, |  (index, search, | (BT-MAC -> prof,| (manifest,     |
|   queue, gapless,  |   smart playlists|  EQ/theme/src   |  A/B, verify,  |
|   crossfade FSM)   |   metadata, art) |  persistence)   |  rollback)     |
+----+----------+----+--------+---------+--------+--------+-------+---------+
     |          |             |                  |                |
     v          v             v                  v                v
+---------+ +--------+ +--------------+   +-------------+   +-----------+
| AudioEng| | Decode | | Metadata/Art |   | Persistence |   | NetSvc    |
| ine     | | ers    | | (tags,covers,|   | (KV store,  |   | (WiFi,    |
| mixer+  | | mp3/aac| |  ASCII gen)  |   |  DB, cache) |   |  TLS,     |
| DSP/EQ+ | | flac.. | +------+-------+   +------+------+   |  HTTP)    |
| RG +SRC | +---+----+        |                  |          +-----+-----+
+----+----+     |             |                  |                |
     |          |             v                  v                |
     |          |        +---------------------------------+      |
     |          +------->|  VFS / Source Abstraction        |     |
     |                   |  (USB FAT, SD FAT, BT-AVRCP-meta) |     |
     |                   +----+----------------+-------------+     |
     v                        |                |                   |
+---------------------------- HARDWARE ABSTRACTION (Circle) ---------------+
| I2S out | USB host | I2C(touch,DAC)| FAT FS | Framebuffer | Mailbox | BT |
|         |          |               |        | (DSI)       | clocks  | HCI|
+---------+----------+---------------+--------+-------------+---------+----+
                                                                      |
                                                             +--------+--------+
                                                             | BTstack         |
                                                             | A2DP sink +     |
                                                             | AVRCP CT/TG     |
                                                             +-----------------+
```

### 1.2 Data flow — the audio render path (the critical path)

```
[Source bytes] -> [Decoder task] -> decode N frames -> [PCM ring buffer (lock-free SPSC)]
                                                              |
                            I2S DMA IRQ / pull callback  <----+
                                       |
                                       v
   [AudioEngine::GetChunk] -> RG gain -> EQ biquads -> crossfade mix -> volume
                                       |
                                       v
                            [I2S DMA double buffer] -> DAC -> speakers
```

- The **I2S pull callback** runs in IRQ/DMA-complete context and must be O(samples) with
  no allocation, no FS, no locks held against slow producers. It pulls already-decoded PCM
  from the ring and applies the fixed-cost DSP chain (gain → biquads → mix → master vol).
- The **decode task** is a normal cooperative `CTask`. It keeps the ring above a low-water
  mark, yielding after each block. It is the only writer (single-producer).
- Two decoders are alive during gapless/crossfade transitions (current + next track),
  feeding either one shared ring (gapless) or two rings mixed at the callback (crossfade).

### 1.3 Threading / scheduling model

Circle's scheduler is **cooperative and non-preemptive**. We map subsystems onto a small
set of `CTask`s with a clear yield discipline. There is exactly one "hard real-time-ish"
context (the I2S DMA callback) and everything else is best-effort cooperative.

| Context | Type | Cadence / yield rule | Priority intent |
|---|---|---|---|
| I2S DMA callback | IRQ/DMA | Per DMA half-buffer (~5–10 ms). Fixed cost. Never blocks. | Hard |
| Decode task | CTask | Fill ring to high-water, then `Yield()`. Block size ≤ 10 ms of audio. | Highest cooperative |
| BTstack run loop | CTask | Pump HCI/L2CAP; A2DP media bytes go to a decode ring. Yield each pass. | High |
| UI/render task | CTask | ~30 fps tick; dirty-region redraw; yields after each frame. | Medium |
| Touch input task | CTask | Poll FT5406 over I2C ~60 Hz; enqueue events. Cheap. | Medium |
| Library indexer task | CTask | Chunked work (one file per slice), `Yield()` between files. Pausable. | Low |
| OTA/Net task | CTask | Only active opportunistically; never during playback-critical windows. | Lowest |

**Golden rules enforced in review:**
1. No task holds the CPU >2 ms without `Yield()` except the I2S callback (which is bounded).
2. Decoders are written/ported to decode in small blocks, not whole-file.
3. The indexer and OTA are *interruptible* — they checkpoint and yield, and back off
   entirely when a track transition (gapless/crossfade) is imminent.
4. Cross-task data crosses via lock-free SPSC ring buffers (audio) or a single-writer
   command queue (UI ↔ controller). Avoid `CSpinLock` on the audio path.

---

## 2. Component Breakdown

For each: responsibilities, key interfaces, source (scratch / Circle / BTstack / port),
complexity, risks.

### 2.1 Boot / Init (`CKernel`)
- **Responsibilities:** Hardware bring-up order, clock config, mount SD, enumerate USB,
  init framebuffer, init I2C/DAC, start scheduler and tasks, load active A/B slot.
- **Interfaces:** Constructs all singletons; provides `CDeviceNameService` lookups.
- **Source:** **Circle** (`CKernel` skeleton from samples). Customize init order.
- **Complexity:** Low–Medium. Mostly wiring; init ordering bugs are the trap.
- **Risks:** Init-order deadlocks; DAC clock vs framebuffer clock contention; R1 DSI.

### 2.2 Display + UI Toolkit
- **Responsibilities:** Framebuffer ownership, double-buffering/page-flip, monospace font
  atlas blitter, character-grid layout engine, widget tree, dirty-region redraw, theme
  (colors, dim/bright), ASCII-art panel rendering, the 7 screens.
- **Interfaces:**
  - `IRenderTarget` over `CBcmFrameBuffer` (raw pixel access).
  - `UI::dispatchTouch(TouchEvent)` from touch task.
  - `UI::onPlayerState(const PlayerState&)` push from controller.
  - `Atlas::drawGlyph(x,y,ch,fg,bg)`; `Grid::cell(col,row)`.
- **Source:** **From scratch** (small, custom). Use Circle's `C2DGraphics` for the
  framebuffer + bouncing-buffer/vsync; build the text/grid toolkit on top. Font atlas
  pre-baked offline from a TTF (see 2.x metadata note); no live TTF rasterizer needed.
- **Complexity:** Medium (toolkit) + Medium (7 screens). The grid model keeps it tractable.
- **Risks:** Tearing without proper double-buffer flip; redraw cost if not dirty-tracked;
  font atlas memory.

### 2.3 Touch Input (FT5406)
- **Responsibilities:** Poll the official panel's FT5406 capacitive controller over I2C
  (addr 0x38), debounce, map raw coords → 800×480, produce down/move/up + simple gestures
  (tap, vertical swipe for lists). Enforce 44×44 px minimum target via UI, not driver.
- **Interfaces:** `Touch::poll() -> vector<TouchPoint>`; emits `TouchEvent` to UI queue.
- **Source:** **From scratch**, thin. Circle provides `CI2CMaster`. The FT5406 register
  map is small and well-documented (touch count + per-point X/Y high/low registers).
  Note: on a stock Pi the firmware exposes touch via a polled memory region; bare-metal we
  talk to the controller directly over I2C on the DSI's I2C bus (see R1/R2).
- **Complexity:** Low–Medium (I2C bus routing for the DSI panel is the only subtlety).
- **Risks:** R2 — the touch controller sits on the DSI connector's I2C, not the GPIO I2C;
  bus identification and pull-ups must be confirmed on hardware.

### 2.4 Audio Engine (mixer + render callback host)
- **Responsibilities:** Own the active sound device **behind an `ISoundDevice` abstraction**
  (PWM onboard jack for bring-up, I2S DAC for production — swappable by config); implement
  the pull callback; host the fixed-cost DSP chain (per-track/album ReplayGain → parametric
  EQ biquads → crossfade mixer → master volume); manage sample-rate switching of the
  device clock; underrun detection + recovery; bit-perfect passthrough when source rate ==
  device rate (I2S path only — PWM is not bit-perfect, see §6/§5).
- **Interfaces:**
  - `ISoundDevice` wraps either `CPWMSoundBaseDevice` or `CI2SSoundBaseDevice` (same pull
    callback contract, different `open(rate,bits,channels)` / clocking impl).
  - `AudioEngine::open(rate, bits, channels)`, `setMasterVolume`, `setReplayGain(g)`.
  - Consumes from `PcmRing` (SPSC). `EngineCallback(int16/32* buf, size_t frames)`.
  - `DSP::setEQ(const EqConfig&)` (atomic pointer swap of coefficient banks).
- **Source:** **From scratch** on top of **Circle**'s `CPWMSoundBaseDevice` /
  `CI2SSoundBaseDevice` + `CDMAChannel`.
- **Complexity:** High. This is the heart; correctness and timing are unforgiving.
- **Risks:** Underruns (R8); sample-rate reclock glitches; fixed-point vs float DSP cost on
  A72 (use NEON-friendly float, see 5.x).

### 2.5 DSP / EQ
- **Responsibilities:** N-band parametric EQ (peaking/shelf), global "car correction" curve
  composed with per-profile curve, coefficient computation (RBJ cookbook biquads), preset
  save/load, real-time coefficient swap without zipper noise (crossfade coeffs or smooth
  gain). Optional: soft limiter post-EQ to prevent clipping from boosts.
- **Interfaces:** `EqConfig{bands[], preamp}`; `Dsp::process(float* L, float* R, n)`;
  `Dsp::compose(global, profile) -> coeff bank`.
- **Source:** **From scratch** (RBJ biquad math is ~100 lines). NEON-vectorize the inner
  loop in Phase 4.
- **Complexity:** Medium. Math is standard; the real-time-safe coefficient swap is the
  subtlety.
- **Risks:** CPU budget under 4-band stereo at 192 kHz; denormals (flush-to-zero on the
  A72 FPU); clipping after boosts.

### 2.6 Bluetooth (BTstack: A2DP sink + AVRCP CT/TG)
- **Responsibilities:** Pair/connect phone, A2DP **sink** (receive SBC, and AAC if we add
  the codec), decode to PCM into a decode ring, AVRCP **controller** (pull metadata: title/
  artist/album/duration, playback status, get cover art via BIP if available) and AVRCP
  **target** (so the phone's lock screen can control us). Expose connected device MAC for
  ProfileManager.
- **Interfaces:**
  - `Bt::onConnected(bd_addr)`, `Bt::onMetadata(TrackMeta)`, `Bt::onA2dpPcm(...)`.
  - `Bt::sendAvrcpCmd(play/pause/next/prev)` (CT). Register as TG for phone control.
  - Provides a `Source` implementation (`BtSource`) to LibraryService for the "BT" source.
- **Source:** **BTstack** (A2DP sink + AVRCP profiles exist as examples: `a2dp_sink_demo`,
  `avrcp_*`). **Circle** provides the HCI transport over the onboard controller +
  firmware-blob loading (reuse Circle's BT transport/firmware path; bridge it to BTstack's
  `hci_transport_t`). SBC decoder ships with BTstack. AAC-over-A2DP needs an AAC decoder
  (we already have one for the library, reuse it).
- **Complexity:** High. Profile state machines + transport bridging + firmware blob.
- **Risks:** R3 firmware blob (CYW43455 .hcd); R4 BTstack↔Circle transport bridge; A2DP
  jitter/clock-drift vs our I2S master clock (resampling/adaptive buffer needed).

### 2.7 USB Host + Filesystem
- **Responsibilities:** Enumerate USB mass-storage (and hubs), expose block devices, mount
  FAT, present a `Source` (`UsbSource`) with directory walk + file read streaming.
- **Interfaces:** `Vfs::open(path)`, `read`, `seek`, `readdir`. `SourceManager` watches
  for attach/detach and notifies LibraryService to (re)index.
- **Source:** **Circle** (`CUSBHCIDevice`, mass-storage class driver, `CFATFileSystem`).
  exFAT support is the gap (R6) — recommend FAT32 media, or port exFAT (see 6.x).
- **Complexity:** Medium (mostly Circle), High if exFAT port is required.
- **Risks:** R5 USB host stability across diverse drives/hubs; R6 exFAT; large-directory
  enumeration latency; hot-unplug mid-read.

### 2.8 Decoders
- **Responsibilities:** Format detection, streaming decode in small blocks, output PCM +
  format (rate/bits/channels), seek, report duration, expose embedded tags + cover art.
  Formats: MP3, AAC, FLAC, ALAC, Opus, WAV (+ stretch DSD, 24/192 PCM).
- **Interfaces:** `IDecoder{ open(stream); decodeBlock(out, maxFrames); seek(ms); format();
  meta(); }`. Factory by sniffing magic bytes / container.
- **Source:** **Ported libraries** (see §6 for exact picks/licenses). All chosen to be
  bare-metal-friendly (no OS, malloc-only): dr_libs (WAV/FLAC/MP3), minimp3, libFLAC or
  dr_flac, faad2/helix-aac or the Opus/AAC combos, libopus + libopusfile, ALAC reference.
- **Complexity:** Medium per decoder (mostly integration + the block/yield wrapper).
- **Risks:** AAC licensing/complexity; ALAC container (MP4) parsing; per-decoder malloc
  patterns vs our heap; CPU for FLAC/Opus at high rates.

### 2.9 Library Indexer + Search
- **Responsibilities:** Background scan of USB/SD sources, parse tags, extract/locate cover
  art, compute ReplayGain on import (R128), populate the library DB, maintain a unified
  search index across all sources (including live BT-AVRCP "now playing"), smart playlists.
- **Interfaces:** `Library::query(filter)`, `Library::search(text) -> results`,
  `Library::onSourceAttached(Source*)`, `Library::smartPlaylist(rule)`.
- **Source:** **From scratch** over the chosen DB (§4). Search = normalized substring +
  token index; not full Lucene.
- **Complexity:** Medium–High (incremental/interruptible scanning is the hard part).
- **Risks:** Scan time on large libraries; SD wear from DB writes (R7); keeping audio
  glitch-free while indexing (yield discipline).

### 2.10 Metadata / Cover-Art / ASCII generator
- **Responsibilities:** Tag parsing (ID3v2, Vorbis comments, MP4 atoms, FLAC blocks),
  cover-art acquisition (embedded → sidecar folder.jpg/cover.png → cache), JPEG/PNG decode,
  downscale to 22×11 cells, luminance→density ramp, dominant-color extraction for tint,
  ASCII-art cache.
- **Interfaces:** `Tags::parse(stream) -> TrackMeta`; `Art::get(albumKey) -> Image`;
  `Ascii::generate(Image) -> AsciiArt`; both heavily cached on disk.
- **Source:** **Ported** (stb_image for JPEG/PNG decode; tag parsing partly from decoder
  libs, partly from scratch). ASCII generator **from scratch**.
- **Complexity:** Medium. ASCII generator is small/fun; tag-format coverage is the grind.
- **Risks:** Malformed tags; huge embedded art images (decode memory); color tint accuracy.

### 2.11 Profile Manager
- **Responsibilities:** Map connected phone BT MAC → profile; load EQ curve, theme variant,
  favorite playlists/tracks, recent-destinations placeholder, last-used source; guest
  fallback; persist to SD.
- **Interfaces:** `Profiles::onBtConnected(bd_addr) -> Profile&`; `Profiles::active()`;
  `Profiles::save()`. Pushes EQ to DSP, theme to UI, source to controller on switch.
- **Source:** **From scratch.**
- **Complexity:** Low–Medium.
- **Risks:** MAC randomization on modern phones (R10) — many phones use resolvable private
  addresses for BLE but a stable Classic address for A2DP; we key on the **Classic** BR/EDR
  address used for A2DP, which is generally stable. Document this assumption.

### 2.12 OTA + Diagnostics
- **Responsibilities:** A/B slot scheme on SD, detect known WiFi (home/garage SSID), fetch
  signed manifest over TLS, verify Ed25519 signature, download package to inactive slot,
  set "try" flag, reboot, confirm health → commit or auto-rollback. Optional encrypted
  crash-log upload.
- **Interfaces:** `Ota::check()`, `Ota::apply(pkg)`, `Ota::commit()/rollback()`;
  `Boot::activeSlot()`, `Boot::markTried()`, `Boot::confirmHealthy()`.
- **Source:** **From scratch** for orchestration; **mbed TLS** (bundled with Circle) for
  TLS + Ed25519/SHA-256; Circle net stack for WiFi/HTTP.
- **Complexity:** High (safe-update correctness; bootloader-level A/B on the Pi is fiddly).
- **Risks:** R11 bricking via bad update; SD boot A/B mechanics on the Pi (the VideoCore
  bootloader reads a fixed config — see §3 Phase 5 for the slot mechanism); signature/key
  management; partial-download corruption.

---

## 3. Phased Implementation Roadmap

Dependencies are explicit. **P-tags** mark work parallelizable once Phase 0 lands.

### Phase 0 — Bring-up (foundation; everything depends on this)
**Goal:** Boots to a touchable terminal-themed shell on the real panel.
1. Circle build system + AArch64 toolchain; minimal `CKernel` boots, serial log alive.
2. Mount SD (FAT), read a config file, list files to serial — proves FS.
3. Framebuffer up on the 7" DSI panel; clear to background color; draw a test glyph.
4. Font atlas pipeline: bake monospace TTF → atlas (offline tool) → blit glyph → render a
   full character grid with dim/bright + accent color.
5. Touch driver: read FT5406 over I2C, draw a cursor/hit-test a button.
6. UI toolkit skeleton: grid layout, one widget (button/list), tab bar, dirty-region draw.
7. Tasking: scheduler running UI + touch tasks at stable ~30 fps.

**Exit criteria:** Can navigate an empty tab bar by touch; stable, no tearing. Serial logs.
**Risks front-loaded here:** R1 (DSI), R2 (touch I2C bus). Validate on hardware *first*.

### Phase 1 — Audio pipeline (depends on P0 FS + scheduler)
**Goal:** Play a WAV/FLAC/MP3 from USB, bit-perfect, no underruns.
1. **[P-A]** Bring up audio out via the **onboard 3.5mm PWM jack** (`CPWMSoundBaseDevice`)
   first — no DAC wiring, fastest path to "sound exists"; play a synthesized sine to verify
   the engine/ring/callback path end to end. Then implement `ISoundDevice` and add the
   **I2S/HiFiBerry DAC+** backend (`CI2SSoundBaseDevice` + PCM5122 I2C config); re-run the
   sine on I2S to verify clocking. From here on, develop against I2S for hi-res/bit-perfect.
2. **[P-A]** PcmRing (lock-free SPSC) + AudioEngine pull callback (passthrough, no DSP yet).
3. USB host + mass storage + FAT mount (Circle); stream a file.
4. Decoder framework + WAV + FLAC (dr_flac) + MP3 (minimp3) first; block-decode + yield.
5. PlayerController: play/pause/stop/seek; feed decode task → ring → engine.
6. Sample-rate switching: reclock I2S to source rate; verify bit-perfect at 44.1/48.
7. Add AAC, ALAC, Opus decoders.
8. Gapless: shared ring, decode next track's header during current track's tail.

**Exit criteria:** Gapless playback of all 6 required formats from USB, no underruns while
UI runs. **Parallelizable:** decoder integration [P-A] can fan out per format.

### Phase 2 — UI fleshed out (depends on P0 toolkit + P1 player state)
**Goal:** The 7 screens, real interaction, library browsing.
1. LibraryService + DB (§4); indexer task scans USB/SD, parses tags (metadata module).
2. Cover-art acquisition + JPEG/PNG decode (stb_image) + ASCII generator + caches.
3. Screens: NowPlaying (3-column art/meta+controls/queue), Library browse (source filter),
   Search (unified), Queue management, Source selector, Settings (stubs), EQ (stub UI).
4. Smart playlists (rule engine over DB).
5. Background indexing UX: progress, non-blocking, interruptible.

**Exit criteria:** Browse/search/queue/play from USB+SD with ASCII art and metadata.
**Parallelizable:** each screen [P-B] is largely independent given the controller API.

### Phase 3 — Bluetooth (depends on P1 audio path; mostly independent of P2)
**Goal:** Phone as a source (A2DP) + metadata (AVRCP CT) + remote control (AVRCP TG).
1. Bridge Circle BT transport → BTstack `hci_transport_t`; load CYW43455 firmware (R3).
2. BTstack run-loop task; pairing flow + persistent link keys (to SD).
3. A2DP sink: SBC decode → decode ring → engine. Handle clock drift (adaptive buffer/SRC).
4. AAC-over-A2DP using existing AAC decoder.
5. AVRCP CT: pull metadata + status → feed BtSource into LibraryService/now-playing.
6. AVRCP TG: phone lock-screen controls drive PlayerController.
7. BtSource integration into unified search/source selector.
8. Expose connected MAC to ProfileManager (Phase 5 consumer; wire the hook now).

**Exit criteria:** Stream from phone, see live metadata + (if available) cover art, control
both directions. **Can run parallel to Phase 2** with a separate engineer.

### Phase 4 — DSP / EQ (depends on P1 engine; touches P2 EQ screen)
**Goal:** Real-time parametric EQ + car-correction + presets, tuned from the UI.
1. RBJ biquad bank; insert into engine callback after ReplayGain, before master volume.
2. Real-time-safe coefficient swap (double-buffer coeff banks, atomic pointer).
3. EQ screen: per-band freq/gain/Q sliders on the grid; live preview.
4. Global car-correction curve + per-profile overlay composition.
5. Preset save/load (§4). NEON-optimize the inner loop; add flush-to-zero; soft limiter.
6. ReplayGain application point finalized (already scanned on import in P2).

**Exit criteria:** Audible, glitch-free EQ; presets persist; CPU headroom verified at 192k.

### Phase 5 — Profiles + OTA (depends on P3 BT MAC, P0 SD, net stack)
**Goal:** Driver profiles by phone proximity; safe A/B OTA.
1. ProfileManager: BT MAC → profile load/apply (EQ, theme, source, favorites); guest.
2. Profile persistence + management UI in Settings.
3. WiFi bring-up (CYW43455 STA); known-SSID detection.
4. A/B slot mechanism on SD (§3 below); boot health-confirm + auto-rollback.
5. Update package format + Ed25519 signing (§4/§6); manifest fetch over mbed TLS;
   verify → write inactive slot → try → commit/rollback.
6. Optional: encrypted crash-log capture + upload.

**Exit criteria:** Insert known phone → profile loads; signed update applies and survives a
forced-bad-boot rollback test.

#### A/B boot mechanism on the Pi (concrete)
The Pi's VideoCore bootloader loads `config.txt` then a kernel image from the boot
partition. Bare-metal A/B options, recommended approach:
- Keep **two kernel images** `kernel8-A.img` / `kernel8-B.img` and a tiny **chainloader**
  as `kernel8.img`. The chainloader reads a `boot_slot` record (a small file or a fixed
  sector) holding `{active, try, tries_left, healthy}`, then loads + jumps to the selected
  image. This avoids depending on `config.txt` rewrites and keeps rollback logic in our
  code. On boot: if `try` set and `tries_left==0` → flip to other slot. App calls
  `confirmHealthy()` after successful start to clear `try`.
- Alternative considered: swapping `config.txt`/`os_prefix` via the firmware. Rejected:
  less control over rollback, riskier to brick, firmware-version-dependent.

### Dependency graph (summary)
```
P0 ──┬─► P1 ──┬─► P2 ─► (P4 EQ screen)
     │        ├─► P3 (BT)  ──► P5 (profiles need P3 MAC)
     │        └─► P4 (DSP)
     └─────────────────────► P5 (OTA needs SD + net)
```
Parallel tracks after P1: {P2 UI}, {P3 BT}, {P4 DSP} by different engineers; P5 last.

---

## 4. Data Structures & Persistence

### 4.1 Library database — **recommendation: custom append-friendly binary store**, not SQLite
**Decision: a simple fixed-schema binary DB with in-RAM indexes, persisted to SD.**
- **Why not SQLite:** It ports to bare-metal (it has a VFS abstraction and is used on
  MCUs), but it pulls in a full SQL engine, a B-tree pager tuned for a real FS, and write
  patterns that chew SD endurance. We don't need ad-hoc SQL; our queries are known
  (by-artist, by-album, by-source, search, smart-playlist rules). The port + memory + wear
  cost outweighs the benefit.
- **Why not flat files only:** Too slow for unified search and joins at library scale
  (10k–50k tracks).
- **Design:**
  - `tracks.db`: array of fixed-size `TrackRecord` (see below), append-only with a free
    list for tombstones. Variable-length strings live in a side `strings.dat` blob,
    referenced by `(offset,len)`; deduplicated for artist/album names.
  - **In-RAM indexes built at boot** from the DB: hash map `trackId→record`, sorted
    indexes for artist/album/title, and an inverted token index (normalized, casefolded,
    accent-stripped) for search. Rebuilt on change; small enough to hold (10k tracks ≈ a
    few MB of index).
  - Writes are **batched and journaled**: stage in RAM, flush periodically + on safe
    points, to limit SD wear (R7). A small write-ahead `journal.dat` gives crash safety.
  - Smart playlists are stored as **rules** (predicate trees over fields), evaluated
    against the in-RAM index, not materialized.

```c
struct TrackRecord {            // fixed 128 bytes, cache/alignment friendly
  uint64_t trackId;            // stable hash of (sourceId, path)
  uint32_t artistRef, albumRef, titleRef; // strings.dat offsets (deduped)
  uint32_t pathRef;            // strings.dat offset
  uint32_t durationMs;
  uint32_t sampleRate;
  uint8_t  bitsPerSample, channels, sourceId, format;
  uint16_t trackNo, discNo, year;
  float    rgTrackGain, rgAlbumGain;  // dB, ReplayGain/R128
  float    rgTrackPeak, rgAlbumPeak;
  uint64_t albumKey;           // hash for cover-art/ASCII cache lookup
  uint32_t flags;              // hasEmbeddedArt, isCueTrack, etc.
  uint8_t  reserved[ ... ];
};
```

### 4.2 SD card layout
```
/boot/                 kernel8.img(chainloader), kernel8-A.img, kernel8-B.img,
                       config.txt, bootcode/firmware, BT firmware blob (.hcd),
                       WiFi firmware + nvram, boot_slot.bin (A/B state)
/carsh/db/             tracks.db, strings.dat, journal.dat, index_cache.bin
/carsh/profiles/       <bdaddr>.prof  (binary), guest.prof, profiles.idx
/carsh/eq/             global_correction.eq, presets/<name>.eq
/carsh/cache/art/      <albumKey>.jpg|png  (original, downscaled variants)
/carsh/cache/ascii/    <albumKey>.ascii    (22x11 cells + tint color)
/carsh/linkkeys/       bt_linkkeys.bin     (paired-device keys)
/carsh/logs/           crash/*.elog (encrypted), runtime.log (rotated, rate-limited)
/carsh/ota/            staged manifest + package during update
```

### 4.3 Profile format (`<bdaddr>.prof`, binary, versioned header)
```
header{magic,version,bdaddr(6),displayName}
eqCurveRef        -> eq preset (inline or name)
themeVariant      (enum)
lastSource        (enum: BT/USB/SD)
favorites[]       -> trackId list
recentDestinations[] (reserved/placeholder for nav)
flags
```

### 4.4 EQ preset format (`.eq`)
```
header{magic,version,bandCount,preampDb}
band[]{ type(peak/lowshelf/highshelf/lowpass/highpass), freqHz, gainDb, q }
```

### 4.5 Cover-art + ASCII cache
- Keyed by `albumKey` (hash of normalized album+albumartist, falling back to folder path).
- Store both the source image (capped dimension, re-encoded) and the derived ASCII art so
  regeneration is rare. ASCII file: `{rows,cols, cells[rows*cols] (char index into ramp),
  tintRGB}`. Aggressive: never recompute if present and source unchanged (mtime/size hash).

---

## 5. Audio Pipeline Design

### 5.1 Buffering & latency targets
- **I2S DMA:** double (ping-pong) buffer, each ~5 ms (e.g. 256 frames @ 48 kHz). DMA-
  complete IRQ triggers the engine pull. Total output latency target **≤ 20 ms**.
- **PcmRing (decoded PCM):** target depth **~500 ms–1 s** of audio (e.g. 250 ms low-water,
  1 s high-water). Deep enough that an indexer/UI hiccup or SD read stall never starves the
  DMA; shallow enough to keep seek/track-change responsive.
- **Decode block size:** ≤ 10 ms of audio per `decodeBlock`, yielding after each, so the
  decode task never monopolizes the CPU.
- **BT A2DP path:** separate jitter buffer (~150–250 ms) before its decode ring, because
  the phone's clock is independent of our I2S master clock.

### 5.2 Sample-rate handling
- **Default: bit-perfect.** When source rate ∈ {44.1, 48, 88.2, 96, 176.4, 192} and the DAC
  supports it, **reclock the I2S/DAC to the source rate** and pass samples untouched.
- **SRC only when forced:** mixing two tracks of different rates during crossfade, or BT
  (fixed 44.1/48 typical) while a local track at another rate is queued, or a DAC that
  can't hit a given rate. Use a good polyphase resampler (recommend a small libsamplerate-
  style SRC or a self-contained windowed-sinc; see §6). Resample the *non-primary* stream
  to the engine's current output rate.
- **Reclock glitch handling:** on track change requiring a rate switch, drain → mute →
  reprogram clock → unmute, hidden inside the gapless/crossfade FSM. Group same-rate tracks
  to avoid audible reclocks where possible.

### 5.3 DSP insertion point
The engine callback chain, per output block, **after decode/ring, before DMA**:
```
ring PCM (float) -> [ReplayGain gain] -> [Parametric EQ biquads] ->
                    [crossfade mixer (if transitioning)] -> [master volume] ->
                    [optional soft limiter] -> int16/int32 -> DMA buffer
```
- **Why post-decode, pre-mix-to-DMA:** EQ/RG must apply to the final stream the user hears,
  per-source; doing it here keeps decoders dumb and the DSP in one fixed-cost place.
- **Format:** convert decoded PCM to **float32** at ring entry; do all DSP in float (A72 has
  fast NEON FP, avoids fixed-point headroom/denormal pain); convert to DAC word size at the
  very end. Enable **flush-to-zero** on the FPU to kill denormal stalls in biquad tails.

### 5.4 Gapless & crossfade
- **Gapless (default):** single output stream. The decode task, on nearing end of track N,
  opens track N+1, strips encoder delay/padding (LAME/iTunSMPB gapless info for MP3/AAC;
  FLAC/Opus carry exact sample counts), and appends N+1's samples to the *same* ring with
  no silence. The transition is invisible to the engine. Requires same rate (else reclock).
- **Crossfade (shuffle/playlist, 0–12 s configurable):** two decoders → two rings; the
  engine mixes with equal-power fade curves over the configured window. CPU: two EQ passes
  during the overlap — budget for it (R8). At rate mismatch, resample the incoming track.
- **Crossfade FSM:** `Steady → PreloadNext → Overlapping → SwapToNext → Steady`. The
  controller drives it; the engine executes the mix with a per-block gain ramp.

### 5.5 ReplayGain application
- **Scanned on import** (Phase 2 indexer) using **EBU R128** (ITU-R BS.1770 loudness) →
  store `rgTrackGain/rgAlbumGain` + peaks in `TrackRecord`. (R128 chosen over legacy RG1.0;
  expose as "ReplayGain 2.0"-compatible since RG2.0 is R128-based.)
- **Applied** at the first stage of the engine chain as a simple linear gain, **track** or
  **album** mode selectable per profile/global. Peak-limit so positive gains can't clip
  (use stored peak to clamp, or rely on the soft limiter).

### 5.6 CPU budget sanity (A72 @ 1.5 GHz, 4 cores — but cooperative single-threaded path)
Note: Circle's scheduler is cooperative on (effectively) one main context for our tasks;
heavy lifting (decode, EQ) shares CPU time. FLAC/Opus decode at 48 kHz is cheap (<5% of one
core). 4-band stereo biquad EQ at 192 kHz is a few % with NEON. The risk is *aggregate*
under crossfade (2 decoders + 2 EQ passes) + indexing + UI; hence the deep ring and the
indexer back-off rule. If needed, pin decode to its own core via Circle's multicore support
(`CMultiCoreSupport`) as a Phase-4 optimization.

---

## 6. Library / Code Recommendations

Legend: **BMF** = bare-metal-friendly (no OS deps, malloc-only, no threads required).

### Hardware: audio output (two-stage)
- **Bring-up: onboard 3.5mm PWM jack.** Zero wiring; Circle `CPWMSoundBaseDevice`. Quality
  is poor (~11-bit effective, PWM noise, no bit-perfect, practically 44.1/48 kHz only) — use
  it only to validate the pipeline in Phase 1, never as the production path.
- **Production: HiFiBerry DAC+ (PCM5122-based).** I2S, up to 24/192, hardware volume,
  documented register map over I2C (`CI2CMaster`). Alternative: Pimoroni/Adafruit PCM5102A
  boards (no I2C control — simpler, but no HW volume). **Pick the PCM5122 (DAC+).** Both
  outputs sit behind `ISoundDevice` so the switch is a config flag.
- **DSD: out of scope.** PCM5122 is PCM-only and DSD adds no value in a car; the hi-res
  ceiling is **24/192 PCM**, which the PCM5122 does natively.

### Audio decoders
| Format | Library | License | Notes |
|---|---|---|---|
| WAV | **dr_wav** (mackron/dr_libs) | Public domain/MIT-0 | BMF, single-header, trivial. |
| FLAC | **dr_flac** | PD/MIT-0 | BMF, single-header. (libFLAC if max compat needed.) |
| MP3 | **minimp3** (lieff) | CC0 | BMF, tiny, fast; has gapless/LAME-tag support. |
| AAC | **faad2** (libfaad) | GPLv2 | OK — project is GPL, so GPL deps are fine. Reuse for A2DP-AAC. (Apache-2.0 ALAC + GPLv2-only faad2 is a theoretical combine-and-redistribute clash; harmless for a GPLv3 project's own use, but if it bites, swap to fdk-aac.) |
| ALAC | **Apple ALAC reference** (alac) | Apache-2.0 | BMF; needs an MP4/M4A demuxer (small) to feed it. |
| Opus | **libopus** + **libopusfile** | BSD | BMF; opusfile needs an Ogg demuxer (libogg, BSD). Higher CPU but fine. |
| (stretch) DSD | — | — | Defer; needs DAC + DoP/native path. |
- **AAC license is the main decision** (§8 open question). Default plan: **faad2** for the
  build, flag the GPL implication; swap to Helix/fdk if licensing requires.

### Sample-rate converter
- **Recommendation:** a self-contained windowed-sinc/polyphase resampler — e.g. the
  **`speexdsp` resampler** (BSD, BMF, small, good quality) or **libsamplerate** (BSD since
  0.1.9). Pick **speexdsp resampler**: smaller, no license friction, plenty good for
  crossfade/BT reclock (we use SRC rarely, not on the bit-perfect path).

### Image decode (cover art)
- **stb_image** (nothings/stb) — public domain, single-header, JPEG/PNG/BMP/GIF. BMF.
  Cap decode dimensions; we only need it downscaled. Pair with **stb_image_resize** for the
  downscale to 22×11 (and to thumbnail cache).

### Tag parsers
- **ID3v2 / Vorbis comments / FLAC blocks / MP4 atoms:** mostly **from scratch** (compact,
  and we already parse containers for ALAC/Opus). Optionally **TagLib** — but it's C++ with
  heavier deps and STL/iostream usage that fights bare-metal; **recommend from-scratch
  parsers** for the handful of tag formats we need. minimp3/dr_flac expose enough for
  embedded art offsets.

### Bluetooth
- **BTstack** (bluekitchen) — dual-license (open-source non-commercial + commercial). A2DP
  sink, AVRCP CT/TG, SBC codec included. **Commercial license likely required for a
  product** (§8 open question). BMF — designed for embedded/no-OS, has a run-loop
  abstraction we map onto a `CTask`.

### TLS / crypto (OTA)
- **mbed TLS** (Apache-2.0) — already used in Circle's ecosystem; BMF. Provides TLS client,
  **Ed25519** + **SHA-256** for manifest/package signing, AES-GCM for encrypted crash logs.
  Alternative: BearSSL (smaller, but mbed TLS integrates more easily with Circle's net).

### Framework
- **Circle** (rsta2/circle) — its own permissive-ish license (see repo; generally usable);
  provides USB, DSI FB, I2S, I2C, FAT, scheduler, net, BT transport. The backbone.

### Font atlas tooling (offline)
- Bake **JetBrains Mono** (OFL) or **IBM Plex Mono** (OFL) to a bitmap atlas with an offline
  tool (stb_truetype or fontbm) at the cell size(s) we need. No runtime TTF rasterizer.
  OFL allows bundling. Accent color `#d97757`.

---

## 7. Risk Register (ranked by severity × likelihood)

| # | Risk | Sev | Lik | Mitigation |
|---|---|---|---|---|
| R1 | **DSI panel init / framebuffer quirks** — bare-metal relies on VideoCore firmware to bring up the official 7" DSI panel; config.txt/`ignore_lcd`, firmware version, and dual-cam/disp quirks can leave no framebuffer. | High | Med | Validate Phase 0 on real hardware first; pin a known-good firmware + `config.txt` (`display_default_lcd`, `dtoverlay=vc4-...` as needed); keep HDMI fallback path for bring-up; document exact firmware blob versions. |
| R2 | **Touch controller on DSI I2C bus** — FT5406 hangs off the DSI connector's I2C (i2c-10/`i2c_vc`), not GPIO I2C-1; wrong bus = no touch. | High | High | Identify correct I2C peripheral early; confirm pull-ups/address 0x38 on scope; abstract the bus behind `Touch` so it's a one-line change. |
| R3 | **Bluetooth firmware blob (CYW43455 .hcd)** — onboard controller needs a vendor HCI firmware patch loaded at init; version mismatch → BT dead. | High | Med | Reuse Circle's proven firmware-load path; pin the exact .hcd from a known Pi firmware; verify with an HCI reset/read-local-version probe before BTstack init. |
| R4 | **BTstack ↔ Circle HCI transport bridge** — gluing BTstack's `hci_transport_t` to Circle's UART/BCM transport + run loop is custom and error-prone. | High | Med | Start from BTstack's posix/embedded transport template; implement the run-loop as a dedicated `CTask`; build an HCI loopback/echo test before profiles. |
| R5 | **USB host stability across drives/hubs** — flaky enumeration, power, or quirky drives cause read stalls → audio underrun or mount failure. | High | Med | Lean on Circle's tested stack; test a matrix of drives/hubs; powered hub recommendation; treat USB reads as fallible (timeouts, retry, deep ring buffer absorbs stalls); never read USB from the audio callback. |
| R6 | **exFAT media** — large USB drives ship exFAT; Circle FAT driver is FAT32 only. **Accepted/closed**: FAT32 mandated (file sizes never near 4 GiB). | Low | Low | Document FAT32-only requirement; provide a "reformat to FAT32" note. Read-only exFAT port deferred indefinitely unless plug-any-drive UX is needed. |
| R7 | **SD card wear from logging/DB writes** — naive per-event writes destroy cards. | Med | Med | Batch + journal DB writes; rate-limit + rotate logs; keep logs/DB in RAM and flush on safe points/shutdown; put cache on USB if available; use endurance-rated SD. |
| R8 | **Audio underruns under load** (crossfade + index + UI + BT simultaneously). | High | Med | Deep PcmRing (≥500 ms); strict yield discipline; indexer/OTA back-off near transitions; budget CPU for 2× decode+EQ during crossfade; consider pinning decode to a second core (`CMultiCoreSupport`) if measured tight. |
| R9 | **Cooperative-scheduler starvation** — any task running too long glitches everything (no preemption). | High | Med | Enforce ≤2 ms/yield rule in code review; chunk all heavy loops; measure worst-case slice with a watchdog timer that logs offenders. |
| R10 | **Phone MAC randomization** breaks profile-by-proximity. | Med | Med | Key profiles on the **Classic BR/EDR** address used for A2DP (stable), not BLE RPA; allow manual profile pick + "remember this device" as fallback. |
| R11 | **OTA bricking** — bad update with no recovery. | High | Low | Chainloader A/B with try/rollback counter; health-confirm handshake; signature verify before write; keep a recovery image; test forced-bad-boot rollback as an exit gate. |
| R12 | **A2DP clock drift vs I2S master** — phone and DAC clocks differ → buffer slowly over/underflows. | Med | High | Adaptive jitter buffer + slow async SRC on the BT stream to track drift; monitor fill level and nudge resample ratio. |
| R13 | **AAC/BTstack licensing** — ~~for a shipped product~~ **RESOLVED**: project is GPL/open source, so faad2 (GPL) and BTstack's free open-source license both apply with no friction or cost. | Low | Low | None needed; keep deps GPL-compatible. |

---

## 8. Open Questions / Decisions Deferred

These need a call (or research spike) **before or during Phase 0**:

1. **Licensing posture — RESOLVED: GPL / open source.** faad2 (GPL) is fine; BTstack's free
   open-source license applies (no commercial license, no cost). Keep all deps GPL-
   compatible (the permissive ones — dr_libs, minimp3, stb, opus, mbed TLS — all are).
2. **exFAT support — RESOLVED: skip it, mandate FAT32.** Audio files never approach the 4 GiB
   limit and firmware/update bins are tens of MB at most, so file size is a non-issue;
   exFAT would only buy plug-any-large-drive convenience. Document the FAT32 requirement for
   media; revisit only if "plug any stock-formatted drive" UX becomes a goal. (R6 downgraded)
3. **DAC + DSD — RESOLVED.** Start on the onboard 3.5mm PWM jack for bring-up, move to the
   HiFiBerry DAC+ (PCM5122) for production, both behind `ISoundDevice`. **DSD dropped**;
   hi-res ceiling is 24/192 PCM (native on the PCM5122).
4. **Cover art over Bluetooth.** AVRCP cover art uses BIP over OBEX — not all phones/
   versions support it. Is BT cover art must-have, or is "metadata text only + generic art"
   acceptable for BT sources? *Recommend: text metadata must-have, BT cover art best-effort.*
5. **OTA transport + key management.** Where do update manifests live (HTTPS endpoint vs
   USB-stick fallback)? Who holds the Ed25519 signing key, and how is the public key
   provisioned/rotated on devices? Need a concrete signing/release process.
6. **WiFi credentials provisioning.** How are home/garage SSIDs + secrets configured —
   pre-baked config file, or a setup screen? (Affects Settings UI + persistence.)
7. **Multicore usage.** Do we commit to `CMultiCoreSupport` (pin decode to a core) from the
   start, or only if Phase 4 measurements show underrun risk? *Recommend: single-core
   cooperative first, measure, escalate only if needed.*
8. **"Driving mode" trigger source.** Confirmed stubbed for now — but what's the eventual
   signal (GPS speed, CAN)? Stub interface shape so it slots in cleanly later.
9. **Library scale target.** 5k? 50k? 200k tracks? Sets DB/index memory budget and whether
   the in-RAM index strategy (§4.1) holds or needs paging.
10. **Crash-log privacy.** Encrypted upload is opt-in by spec — confirm consent model and
    what's captured (no PII, no library contents?).

---

## Appendix A — Module → Source-of-truth quick map

| Module | Built from | Complexity |
|---|---|---|
| Boot/init | Circle (customize) | Low–Med |
| Display + UI toolkit | Scratch (on Circle FB) | Med |
| Touch (FT5406) | Scratch (on Circle I2C) | Low–Med |
| Audio engine | Scratch (on Circle I2S/DMA) | High |
| DSP/EQ | Scratch (RBJ + NEON) | Med |
| Decoders | Ported (dr_libs/minimp3/faad2/opus/alac) | Med |
| Bluetooth | BTstack + Circle transport | High |
| USB + FAT | Circle | Med |
| Library/search | Scratch (custom DB) | Med–High |
| Metadata/art/ASCII | Ported (stb_image) + scratch | Med |
| Profiles | Scratch | Low–Med |
| OTA + net | Scratch + mbed TLS + Circle net | High |

## Appendix B — First two weeks (concrete starting moves)

1. Clone Circle, build the AArch64 toolchain, flash the `hello` sample to confirm the
   build/flash loop on the actual Pi 4 + serial console.
2. Get the **framebuffer + a single glyph** on the **real 7" DSI panel** (resolve R1 now).
3. Get **FT5406 touch** reading over the correct I2C bus (resolve R2 now).
4. Get a **sine wave out the HiFiBerry DAC** via `CI2SSoundBaseDevice` (resolve I2S/clock).
5. Only then build the UI toolkit and PcmRing. R1, R2, R3 are the bring-up gates — burn them
   down before writing application logic.
```
