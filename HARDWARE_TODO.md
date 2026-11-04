# Hardware TODO — bench validation backlog

Each item is a task that **cannot be meaningfully validated without the physical
board/peripheral**. Built behind its interface and host-tested where possible;
this is the prioritized list for the first hardware session. Format:

`Hn — <module/file>` · **Validate:** <exact step> · **Stub now:** <current behavior>

**Next bench session: start with H9** — the checklist for everything fixed on
`claude/review-fixes` (2026-09-25). None of it has run on the Pi yet.

---

### H0 — Circle image build + board boot  [DONE — boots on real Pi 4]
- **Module:** build infra, `CKernel`.
- **Done:** ARM `aarch64-none-elf` 14.2 toolchain + circle-stdlib pinned
  (`scripts/fetch_circle.sh`) and built (`STDLIB_SUPPORT=3`: newlib + libstdc++ —
  required because the core uses the C++ STL). `kernel8-rpi4.img` **boots on a
  real Pi 4** and runs the carsh UI — biggest risk (STL core on the target ABI)
  retired on hardware, not just QEMU.
- **Confirmed on the bench:** flashed via `scripts/make_sdcard.fish` (FAT32 boot
  partition + firmware + Pi 4 `config.txt`); the media-player UI renders on the
  panel and the boot log streams over serial. The remaining Circle backends below
  are now wired (Vfs/touch/PWM-audio/decode all running on the board).

### H1 — DSI panel / framebuffer bring-up (Risk R1)  [DONE]
- **Module:** `CKernel`, IRenderTarget Circle backend.
- **Done:** VideoCore brings up the framebuffer from firmware + `config.txt`; the
  full UI renders on a DSI panel. (The official 7" that appeared dead was a loose
  ribbon cable, not firmware — re-seating fixed it; bring-up was confirmed on the
  spare ESP32-P4 dev-kit DSI panel in the meantime.)

### H2 — Touch reports points (Risk R2)  [DONE]
- **Module:** Touch Circle backend (`CRPiTouchScreen`).
- **Done:** `CRPiTouchScreen` reports points on the panel and tap/swipe drive the
  UI. At validation time `CircleTouch` pumped `Update()` once per UI frame (~10-25
  Hz, not the >60 Hz this entry used to claim — it lost short taps and turned flicks
  into taps, review T4); since 2026-09-25 it is pumped at 100 Hz from the CTimer
  periodic IRQ — re-bench in H9 §C. Bug found+fixed on the way: the driver reports
  FingerUp at (0,0), which misclassified taps as swipes — `GestureRecognizer` now
  uses the last known position on Up.

### H3 — Audio out: PWM jack [DONE], I2S DAC [pending HAT]
- **Module:** AudioEngine Circle backends.
- **Done (PWM):** `CirclePwmSoundDevice` drives the onboard PWM → 3.5 mm jack;
  audible playback confirmed. Lazy-opens at the track's rate, safe close
  (Cancel + bounded drain before delete, pre-allocated IRQ scratch), peak meter.
- **Pending (I2S, needs a DAC HAT):** play a sine out a PCM5122/HiFiBerry DAC+;
  verify reclock at 44.1/48/96/192 kHz and bit-perfect passthrough.

### H4 — Real-time / underrun behavior under load (Risk R8)
- **Module:** AudioEngine, decode task, scheduler yield discipline.
- **Done (AA path, 2026-06-29):** AA media-audio underruns root-caused on the bench
  (serial `aa-audio`/`aa-decode` instrumentation) and fixed — H.264 decode moved to a
  **dedicated A72 core** (`device/CircleAaDecodeCore.h`, `CMultiCoreSupport`), audio
  refilled several times per UI loop, `JitterBuffer` prebuffer + quick-resume absorb
  the dongle's bursty delivery (PR #24). Cross-core handoffs are lock-free SPSC
  (`src/aa/AuQueue.h`, `src/aa/FrameRelay.h`).
- **Validate (local player):** confirm no underruns with crossfade + indexer + UI + BT
  live; measure worst-case cooperative slice.
- **Stub now:** Host pump is synchronous/deterministic; underrun counter exists but
  cannot exercise real DMA timing.

### H5 — USB host + FAT across drives/hubs (Risk R5)  [DONE — single drive]
- **Module:** Vfs Circle backend (`CUSBHCIDevice` + `CFATFileSystem`).
- **Done:** A real USB FAT32 stick mounts on the xHCI host port and the Indexer
  scans it — albums/tracks populate the library, selecting a song loads it into
  the player (length shown). Hidden dot-folders (e.g. `.Trash-1000`) are skipped.
- **Remaining (not blocked):** widen to a matrix of drives/hubs; hot-unplug
  mid-read.

### H6 — Bluetooth A2DP sink: WORKING on hardware (phone → Pi jack)
- **Done (UB500 / RTL8761BU, branch `device/bt-dongle-bringup`):** a phone pairs and
  plays music out the Pi's 3.5 mm jack. Firmware loads from SD (product-id +
  `hci_enable_custom_pre_init`), HCI event reassembly + USB-IRQ trampoline, USB ACL
  path (`CUSBBluetoothDevice` patch under `scripts/patches/`), SBC media-header strip
  → PWM output at the SBC rate.
- **AVRCP fixed in code 2026-09-25 (review B1-B4), `[hw]` pending — H9 §F.** It had
  never worked: the AVRCP packet handlers were unregistered (the first notification
  halted the board) and no SDP records were published. Absolute volume came with it.
- **Remaining polish (deferred — BT left "basically functional" by owner, 2026-06-27):**
  1. ~~**BT volume → engine.**~~ Done: `BtMixSink` applies the HU master gain (the
     rail) to the A2DP PCM; since 2026-09-25 the phone's AVRCP absolute volume is
     applied ahead of it.
  2. **Bonding persists across boots.** Save link keys / pairing to SD so a paired
     phone reconnects after a power-cycle without re-pairing (`btstack_tlv` on SD).
     Today no link-key DB is installed at all, so a paired phone cannot reconnect
     even within the same boot without re-pairing (review B5).
  3. **Scan screen shows names AND MACs.** `DevicesScreen` should list the friendly
     device name *and* the BD_ADDR (GAP inquiry returns both; today connect is by MAC).
  4. **Incoming-pair approval popup.** When the phone requests pairing, show an
     accept/reject prompt on the carsh screen (SSP confirmation) instead of silently
     accepting — wire BTstack's pairing/IO-cap callbacks to a UI dialog.
  5. **Now-playing display.** Song title, artist/album, and album art via AVRCP
     metadata + cover-art. The text + play state path is wired since 2026-09-25
     (`[hw]` H9 §F); album art is still open.
  - Plus: onboard CYW43455 transport (stage 2). Original notes:

### H6 (orig) — Bluetooth: USB ACL path + firmware blob + bring-up (Risks R3/R4/R12)
- **Module:** BTstack↔Circle HCI transport, A2DP sink, AVRCP CT/TG.
- **Compiles now:** `make CARSH_HAVE_BTSTACK=1` builds + links BTstack into the
  image (Classic A2DP-sink + AVRCP + SBC, embedded run loop, Circle USB transport,
  Realtek chipset). The default `make` still links a stub. The single biggest
  unknown — does BTstack co-compile with Circle on the target ABI — is retired.
  AVRCP now-playing assembly is host-tested (`tests/bt/BtParse_test`).
- **Bench validate (stage 1, USB dongle — Realtek RTL8761B, e.g. TP-Link UB500):**
  1. **Close the USB ACL gap (blocker for audio).** Extend Circle's
     `CUSBBluetoothDevice` with bulk ACL in/out and wire it into
     `CircleHciTransport.cpp` (currently HCI command/event only; ACL is dropped).
     `third_party/circle-stdlib/` is git-ignored → ship the extension as a tracked
     patch under `scripts/`.
  2. Place the Realtek `rtlbt_fw`/`rtlbt_config` for the dongle's USB PID under
     `SD:/firmware/rtlbt` (R3); confirm HCI reset / read-local-version.
  3. Trampoline HCI events from USB-completion (IRQ) context onto the run loop.
  4. Pair a phone; A2DP SBC → `g_btRing`; AVRCP metadata + play/pause/next both
     ways; clock-drift adaptive SRC on the PCM (R12).
- **Stage 2:** port the transport to the onboard CYW43455 (UART + `.hcd` firmware).
- **Stub now (default build):** BtSource and AVRCP are interface stubs;
  `btGlueInit` returns false so no `CircleBtRunLoopTask` is spawned.

### H7 — WiFi STA + OTA flash/rollback (Risk R11)
- **Module:** NetSvc (CYW43455 STA), OTA flash writer, chainloader A/B.
- **Validate:** Known-SSID detect; HTTPS manifest fetch; write inactive slot; forced-
  bad-boot rollback survives. The *flash write* and *chainloader jump* are HW-only.
- **Stub now:** OTA package parse/verify + A/B state-machine decision logic are
  host-tested; the actual slot write + reboot are stubbed behind an interface.

### H7b — Serial OTA push + A/B chainloader (the no-card-swap dev loop)
- **Module:** `device/chainloader/`, `device/OtaPush.h`, `CircleUsbLog` RX feed,
  `scripts/carsh_ota.py`. All decision/verify/framing logic is host-tested; this
  session validates the device plumbing.
- **One-time:** migrate the card (chainloader as boot kernel, app as `kernel8-A.img`
  — see `device/chainloader/README.md`); `carsh_ota.py keygen` + rebuild to provision
  the push key. Since 2026-09-25 the chainloader needs `chainboot-multicore.patch`
  (the unpatched one halted in `EnableChainBoot()`, review O1), and the app on the card
  must be from 2026-09-25 or later: older kernels never ack the flow-controlled
  push. So the migration is the one unavoidable card swap — H9 §G covers it.
- **Validate:**
  1. Chainloader boots slot A on the migrated card (UART log: "chainboot slot A").
  2. `carsh_ota.py flash` end-to-end: push → `ota-push: apply=ok` → reboot →
     "trial CONFIRMED healthy" → new image runs. Note the transfer rate.
  3. Forced rollback: push an image built to hang before the UI loop; power-cycle
     3×; expect "ROLLED BACK to slot A" and the old image running.
  4. Push while AA is streaming (CDC RX load vs the xHCI dongle path — they share
     no controller, but confirm no audio/video hiccups).
  5. `boot_slot.bin` power-loss: yank power mid-apply; expect a clean rescan/boot
     (the record is written after the slot image, so a loss before that just means
     re-sending the push). Known gap (review O3, open): the record is
     truncated-then-rewritten — and the chainloader rewrites it on every boot — so a
     power loss *during that write* leaves an empty record, which the chainloader
     treats as a fresh card (slot A, healthy).

### H8 — Android Auto stability + video throughput  [partly done — TOP open AA bug]
- **Module:** `CircleAoaTransport`, Circle xHCI, `CircleAaDecodeCore`.
- **Done (2026-06-29):** AA video + media audio play end-to-end; choppy audio fixed
  (H4) and the 720p decode-can't-keep-real-time screen freeze fixed by dropping to
  480p (native panel) — see `AA_AUDIO_INVESTIGATION.md`, PR #24.
- **FIXED in code (2026-09-25, review A1), bench pending — the hard hang.** The
  "AA doesn't start up stable" hang (serial silent after `xhciep: Transfer error 4
  on endpoint 3`, USB-C still enumerated) was carsh's own bug: the bulk-IN
  completion called `GetResultLength()` on the failed URB, which is
  `assert(m_bStatus)` in Circle Step51, so the assert fired inside the xHCI ISR. Its
  panic message never reached either console: both are interrupt-driven, and the
  board halted inside the ISR. Now the error latches the link dead (`AaBulkLink`) → the session ends →
  the dongle re-cycles by itself, or is force-re-enumerated after 3 s
  (`AaLinkSupervisor`; max 3 in a row, then `AA link lost — replug dongle`). A TLS
  desync from lost bulk-IN bytes surfaces as `state -> Shutdown: TLS failure`
  instead of a silent wedge. Re-bench: H9 §B.
- **OPEN — why the dongle raises `Transfer error 4` during the TLS handshake.**
  Non-deterministic: usually AA connects fine, but sometimes it stalls in
  `SslHandshake` (the historic log showed `rx` stuck at 12 — just the
  VERSION_RESPONSE — with `inFlight=2`) for seconds, then the bulk-IN fails with
  completion code 4 (USB Transaction Error). It now costs a session restart, not the
  board. Same class as the 2026-06-28 Running-phase bulk resets (see the
  `CircleAoaTransport` row in `STATUS.md`).
  - **Repro/observe:** `python3 scripts/bench_capture.py`; watch for `rx=12` stuck in
    SslHandshake → `Transfer error 4 on endpoint 3` → `aa: link dead: bulk-IN
    transfer error` → recovery (H9 §B.2).
  - **Next:** usbmon/OpenAuto reference capture on a Linux PC vs the Circle-xHCI path;
    confirm it's not provoked by the SMP enable (compare `CARSH_NO_DECODE_CORE=1`);
    consider a TLS-handshake stall watchdog (a stall with no USB error still waits
    forever). Resetting/re-posting the halted IN endpoint in place is not possible on
    Circle Step51's xHCI without a Circle patch — see the comment above
    `aoaForceReenumerate` in `device/CircleAoaTransport.cpp`.
- **OPEN — video lag under heavy motion.** 480p decode is real-time (~15 ms) but there
  is no frame-drop policy, so a sustained high-motion scene can still build queue
  latency. Add "drop to the latest IDR when behind" in the decode pump.

### H9 — Bench checklist: `claude/review-fixes` (2026-09-25)  [NOTHING BENCHED YET]
- **Scope:** everything this branch fixed after the 2026-09-25 code review. All of it
  is host-done (469 host tests pass) and cross-builds in all six device variants;
  none of it has run on the Pi. Expected log lines are quoted as the code prints
  them; the review IDs (T1, A1, ...) match the commit messages.
- **Prep.**
  1. circle-stdlib with all seven `scripts/patches/` + the manual gadget VID edit, and a
     **full libcircle rebuild** (the two sysconfig patches change `sysconfig.h`) —
     `scripts/patches/README.md`. (The app no longer builds without the heap-bucket
     patch; a patched header over a stale `libcircle.a` is not caught, so rebuild.) Build `cd device && make clean && make
     CARSH_HAVE_OPENH264=1 CARSH_HAVE_BTSTACK=1` (`make clean` whenever the `CARSH_*`
     flags change: the Makefile doesn't track them) and `device/chainloader`.
  2. **One card swap is unavoidable.** Kernels built before this branch never ack a
     push (the new `carsh_ota.py` stops with `no ack for BEGIN`), and the old
     chainloader halted in `EnableChainBoot()`. Install the new chainloader as
     `kernel8-rpi4.img` and this app as `kernel8-A.img` (`device/chainloader/README.md`);
     every later image can be pushed (§G).
  3. For a push, stop `bench_capture.py` and use `carsh_ota.py flash --log bench.log`.
- **A. Android Auto input** (T1-T3, T10)
  1. In the projection: tap a button, scroll a list (drag), pan Maps, long-press (drop
     a pin). The phone acts at the right spot; press feedback appears at finger-down;
     a scroll does not activate the row under the lift point; fling speed is sane.
     Slide a finger off the video into the pillarbox/tab bar: the drag continues
     along the video edge.
  2. On the AA tab, tab-bar taps and the volume rail work and send nothing to the phone.
  3. With AA audio playing, Now Playing Play/Pause, Next and Prev act on the phone.
     After PAUSE, Now Playing stays ANDROID AUTO with '>', and '>' resumes AA.
  4. Tap the AA mic (or "Hey Google"): the log shows `tx frame ch=7 id=8006` and the
     voice session times out on silence (no mic uplink) instead of hanging.
  5. AA touch + keys still work after > 72 min of uptime. (All stamps now come from
     carsh's own counter clock, `device::clockUs`, which doesn't have Circle's ~94.9 h
     overflow; a > 95 h soak is optional.)
- **B. AA link faults + H8 re-bench** (A1, A3-A5, A7, A9). Removals are noticed only
  when the kernel pumps USB plug-and-play (every 33 UI frames), so `~N ms` figures are
  upper bounds, coarse by about a second.
  1. Hot-unplug the dongle mid-session. **The board must survive.** Expect possibly
     `xhciep: Transfer error N on endpoint 3` and `aa: link dead: ...`, then either
     `uaa1 gone — session ended (unplugged, or the dongle re-cycled mid-session)` or
     `session ended (<fault>) — tearing down; waiting 3000 ms ...` followed by
     `uaa1 gone ~N ms after the session ended — ...`. The UI returns to Now Playing (no
     frozen last frame; a finger held on the video fires no Now Playing button on
     lift). Replug: `accessory bulk endpoints up — AA transport ready` → `uaa1 bound —
     sending VERSION_REQUEST`, and AA auto-shows.
  2. H8 repro loop (power-cycle / replug until the TLS-handshake stall hits). It must
     never hard-hang again. Expect `link dead: bulk-IN transfer error` → `session ended
     (...) — tearing down; waiting 3000 ms for the dongle to re-cycle`, then either
     `uaa1 gone ~N ms after the session ended — the dongle re-cycled by itself ...`
     or `dongle did not re-cycle — forcing USB re-enumeration (1/3)` → `re-enumeration:
     bus rescanned` → `uaa1 gone — removed by the forced re-enumeration (1/3) ...`;
     then a new `uaa1 bound` session that reaches Running. Check the USB stick and BT
     dongle survive the rescan and time the UI stall (a few hundred ms expected).
     `AA link lost — replug dongle` must appear only after 3 failed forced attempts or
     15 s with nothing back. `uaa1 re-bound (same transport, ...)` means Circle could
     not remove the device and the same transport is being retried.
  3. Choose Exit in AA on the phone while projecting: `tx frame ch=0 id=0010 enc=1`,
     `state -> Shutdown: phone exit (SHUTDOWN_REQUEST)`, `session ended (...) — tearing
     down; no forced re-enumeration ...`, UI back on Now Playing, and **no**
     `forcing USB re-enumeration`. Record whether the CA525 re-cycles by itself
     (`uaa1 gone ~N ms after the phone exit ...` — note N) or keeps the accessory
     connection (no `uaa1 gone` within ~30 s: AA then needs a replug, which decides
     whether a user-initiated AA restart is needed).
  4. A mid-session bulk-IN error or TLS desync shows `link dead` or `state -> Shutdown:
     TLS failure`, never a silent video freeze with the state stuck at Running.
  5. If the dongle's OUT can be stalled > 3 s (drop its WiFi to the phone while the HU
     sends): exactly one `link dead: bulk-OUT failed/timed out after ~3000 ms`, no
     repeated 3 s freezes, then recovery as in 2.
- **C. Touch** (T4-T9)
  1. On Lib/Queue, 40 ms taps and 80 ms / 150 px flicks register (flicks scroll, never
     select), also on the AA tab while video plays; two taps 50 ms apart both
     register. `touch: ring dropped` never appears in normal use; no PWM/AA underrun
     regression from the timer IRQ (`u=`/`au=` on screen on the non-AA tabs,
     `aa-audio: ... under=` in the log).
  2. Two fingers: confirm the FT5406 reports distinct ids per contact. A palm/thumb on
     the rail while scrolling a list leaves the volume alone; a stray second contact
     doesn't steal a tap. (By design a thumb resting on the rail blocks taps elsewhere
     until it lifts — decide whether that is acceptable.)
  3. A Lib list drag that lifts on PLAY ALL or the header still scrolls and PLAY
     doesn't fire; on Q/Set/BT a drag that lifts on the tab bar scrolls without
     switching tabs.
  4. A partially scrolled list draws no row fragment over the Library header or the
     BACK/PLAY/SHUFFLE row; long titles end in `..` before the A-Z strip.
  5. Diagnostics corner: the A-Z strip's 'A' is tappable; the top-right marker (20×20
     hot zone, was 60×60) still toggles the diag panel on non-AA tabs; on the AA tab
     the marker is hidden and a top-right tap reaches the phone, and nothing (no OUT
     meter, no `u=`/`au=` text) is drawn over the bottom of the video.
  6. Keep a finger on a Library row while AA's first frame auto-shows the AA tab: the
     phone must not receive a tap.
  7. Touch-loss recovery (needs a temporary debug stall ≥ 1.5 s in the UI loop): drag
     one finger for > 1.1 s, then tap 8+ times (or use several fingers) before the
     stall ends. Expect exactly one `touch: ring lost a Down/Up (N total); input reset`
     and working taps right after. A single-finger drag alone only logs `touch: ring
     dropped` and must not reset the contact. Normal use never logs the reset.
- **D. Local playback + EQ** (P1-P5)
  1. A mono MP3/WAV (e.g. a podcast) and a 5.1 FLAC/WAV play cleanly, at the right
     pitch, on both channels; no crash or hang afterwards.
  2. Mixed-rate queue 44.1 → 48 → 44.1 kHz (or SHUFFLE ALL over a mixed library):
     playback continues and the log shows `audio: PWM jack @ <rate> Hz (local)` at each
     change; note how audible the ~100-120 ms start-of-track pitch blip is. **Repeat
     with a BT phone connected but idle** (and with an AA session up, not playing):
     the jack must still follow the local track (before, it ran at the phone's rate,
     so 48 kHz tracks played 8 % slow). A `.m4a`/`.opus`
     queued between two FLACs is skipped (neither decoder is in the device image).
  3. A < 1 s track with ≥ 2 tracks after it (tap it, or next/prev onto it): all play
     through, no crash.
  4. The EQ tab shows 10 bars labelled 31..16000; each tap moves a band ±1 dB,
     audible on local playback (AA/BT audio bypasses the EQ, as before). Flat EQ: no
     audible change and no PWM underruns from the 10-biquad chain in the IRQ. With a
     band boosted, play a tone/sweep file at 44.1 kHz and at 48 kHz: the boost sits at
     the labelled frequency on both (it sat 8 % low at 44.1 kHz). A 22.05 kHz file
     with the 16 kHz band boosted plays clean (that band is passed through).
- **E. Media session** (A2, P4)
  1. Play a USB track, then start media on an AVRCP phone: the USB track pauses at
     once (`audio: local player paused (Bluetooth took over)`). Pause the phone:
     silence, the USB track does NOT resume, Now Playing stays on BLUETOOTH. Pick a
     library track while the phone streams: local plays and the phone gets an AVRCP
     pause. Repeat with AA media: there the takeover comes ~6 s after the AA audio
     starts (`MediaSession::kTakeoverMs`; AA is heard over the USB track meanwhile).
  2. AVRCP phone: play from the phone, press pause on Now Playing, then '>' within
     1-3 s: the phone resumes, and the button shows '>' as soon as the pause lands (no
     flicker back to pause during the A2DP silence tail).
  3. Pause media on the phone, then trigger a nav prompt: Now Playing (BLUETOOTH)
     stays on '>' and tapping it resumes the media.
  4. Disconnect the phone while it plays (BT off / out of range), then let it
     reconnect with a local track playing: the local track is not paused by a stale
     PLAYING and Now Playing shows no old phone title until the phone plays.
  5. A phone without AVRCP (if available): the pause button shows while it streams
     and does nothing (expected — no AVRCP channel); it takes the session ~6 s after
     its stream starts, like AA.
  6. **Chimes and prompts must not take the session** (final review). USB track
     playing, phone connected with its media paused: a notification sound, then a
     Maps prompt over A2DP. Each is heard over the USB track; the USB track keeps
     playing afterwards, Now Playing stays on it, and no `local player paused` line
     appears. Same while projecting AA with AA media paused: a guidance prompt plays
     over the USB track and leaves it playing. Record whether AA prompts arrive as
     media audio (`aa-audio: ... eng=1` during the prompt) and how long the longest
     prompts run: a sound that lasts ≥ ~5 s (e.g. an Assistant answer) does take the
     session.
  7. Rate during a chime: USB track at 48 kHz, phone at 44.1 kHz. The log shows
     `audio: PWM jack @ 44100 Hz (Bluetooth)` while the chime plays and `@ 48000 Hz
     (local)` ~1 s after it ends; the chime's pitch is right. Note how audible the two
     reclocks are (a short gap each).
- **F. Bluetooth** (B1-B4, B8, absolute volume)
  1. Boot log: four `SDP ... record registered` lines.
  2. Pair and stream: `AVRCP: connected to <phone>` within ~2 s of `A2DP signaling
     up` (the phone opens it, or the HU logs `AVRCP: connecting to` ~1.5 s in). No halt
     with AVRCP up, including the phone's own media keys.
  3. Now Playing (BT) shows title + artist; the play/pause glyph matches the phone,
     also after an auto-advance (B4); HU play/pause/next/prev drive the phone. (After
     an HU Next/Prev the title can lag until the next change — B7, open.)
  4. Absolute volume: moving the phone's slider logs `AVRCP TG: phone volume N/127
     (P%)` and carsh's BT loudness follows it (same loudness at the same slider
     position as before absolute volume); the HU rail still scales on top; phone at 0
     = silent. Disconnect the phone: the next connect starts at unity, no stuck-quiet
     level. Try an iPhone too if one is available.
  5. CONNECT on an off/out-of-range phone: `ACL link FAILED to ... (status 0x04 ...)`
     and `BT connect failed: 0x4`; the row is not marked, no DISCONN button, PWM rate
     unchanged. DISCONN while streaming with AVRCP up: `ACL link down` and the row
     unmarked at once. Double-tap CONNECT: `already up or in progress`, no halt. With
     two paired phones, the second auto-connecting must not switch the profile.
  6. Connect from the phone's own Bluetooth settings (the A2DP-sink record is
     advertised now). Reconnect after link loss / reboot still needs B5 (H6 item 2).
  7. Connect a phone after > 72 min of uptime: no ~71 min stall (B8).
  8. AVRCP released while A2DP keeps playing (`AVRCP: released` with no `ACL link
     down`; if it can be provoked, e.g. by the phone's per-device media-control toggle):
     the loudness does not change (the phone's absolute volume stays applied), and Now
     Playing stays BLUETOOTH showing '||' while the music plays (not a stale '>'); it
     goes back to AVRCP when AVRCP reconnects.
- **G. OTA push + chainloader** (O1, O2, A11) — then run H7b.
  1. After the Prep card swap, the GPIO UART shows `chainboot slot A ...` and the app
     boots; the chain-booted app still starts its decode core (no `CPU core N does not
     respond` / `did not start` from mcore) and AA video decodes.
  2. `carsh_ota.py flash --log bench.log` with a multi-MB `CARSH_HAVE_OPENH264=1` image
     while AA streams: no `CDC RX overrun` lines, success on the first attempt, then
     `ota-push: rebooting` and a clean reboot into the new slot (`trial CONFIRMED
     healthy`), not a halt (A11). Note the KiB/s (the host simulation gave ~1.5 MiB/s
     at ~1 ms RTT; several hundred KiB/s is fine) and whether ~1 ack line per 4 KiB in
     the console is acceptable.
  3. With `bench_capture.py` running, `carsh_ota.py push` refuses and names its pid.
  4. Type into the port mid-push: `CDC RX overrun: host->device bytes dropped`,
     `ota-push: recv failed err=rx-overrun`, an automatic restart from BEGIN, no halt.
- **H. Heap with large covers** (L1). Add a temporary log of
  `CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW)` at boot, after scanning a stick
  with 100+ files carrying ≥ 1 MiB embedded art, and after 20+ distinct-album art
  loads with 1000-3000 px `cover.jpg`: free space plateaus after warm-up instead of
  falling per file/album; the scan finishes and `library.db` is saved. Time a
  3000 px cover decode (still synchronous in the UI loop; ~0.5-1.5 s stall expected);
  a `cover.jpg` over 3000 px falls back to `folder.jpg`; tags and art flags look right
  for real store downloads.
- **Fixed in code: Circle's 64-bit clock overflowed after ~95 h.** With
  `USE_PHYSICAL_COUNTER` (always on for the Pi 4), Step51's `CTimer::GetClockTicks64()`
  computes `CNTPCT * 1000000 / CNTFRQ` in u64, which overflows at the Pi 4's 54 MHz
  after ~94.9 h of uptime and jumps back to ~0. carsh now reads `CNTPCT_EL0` /
  `CNTFRQ_EL0` itself (`device/CircleClock.h`) and converts as `(cnt / frq) * 1000000
  + (cnt % frq) * 1000000 / frq` (host-tested past the overflow point) for every
  timestamp and interval it takes: touch and AA input stamps, the media session,
  BTstack's `hal_time_ms`, the AA link supervisor and the diagnostic timers. Bench:
  nothing to see short of a > 95 h soak (optional); A.5 and F.7 cover the 72-min
  mark.
