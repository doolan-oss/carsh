# Hardware TODO — bench validation backlog

Each item is a task that **cannot be meaningfully validated without the physical
board/peripheral**. Built behind its interface and host-tested where possible;
this is the prioritized list for the first hardware session. Format:

`Hn — <module/file>` · **Validate:** <exact step> · **Stub now:** <current behavior>

---

### H0 — Circle image build + board boot  [DONE — boots on real Pi 4]
- **Module:** build infra, `CKernel`.
- **Done:** ARM `aarch64-none-elf` 14.2 toolchain + circle-stdlib pinned
  (`scripts/fetch_circle.sh`) and built (`STDLIB_SUPPORT=4`: newlib + libstdc++ —
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
- **Done:** `CRPiTouchScreen` reports points on the panel; `CircleTouch` pumps
  `Update()` >60 Hz and tap/swipe drive the UI. Bug found+fixed on the way: the
  driver reports FingerUp at (0,0), which misclassified taps as swipes —
  `GestureRecognizer` now uses the last known position on Up.

### H3 — Audio out: PWM jack [DONE], I2S DAC [pending HAT]
- **Module:** AudioEngine Circle backends.
- **Done (PWM):** `CirclePwmSoundDevice` drives the onboard PWM → 3.5 mm jack;
  audible playback confirmed. Lazy-opens at the track's rate, safe close
  (Cancel + bounded drain before delete, pre-allocated IRQ scratch), peak meter.
- **Pending (I2S, needs a DAC HAT):** play a sine out a PCM5122/HiFiBerry DAC+;
  verify reclock at 44.1/48/96/192 kHz and bit-perfect passthrough.

### H4 — Real-time / underrun behavior under load (Risk R8)
- **Module:** AudioEngine, decode task, scheduler yield discipline.
- **Validate:** Confirm no underruns with crossfade + indexer + UI + BT live;
  measure worst-case cooperative slice; consider `CMultiCoreSupport` for decode.
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
- **Remaining polish (deferred — BT left "basically functional" by owner, 2026-06-27):**
  1. **BT volume → engine.** The volume rail has *no effect* on BT playback today;
     only the phone's own volume changes it. Route the BT PCM (`g_btRing`) through
     the engine gain stage (or apply gain in `BtMixSink`) so the rail controls it.
  2. **Bonding persists across boots.** Save link keys / pairing to SD so a paired
     phone reconnects after a power-cycle without re-pairing (`btstack_tlv` on SD).
  3. **Scan screen shows names AND MACs.** `DevicesScreen` should list the friendly
     device name *and* the BD_ADDR (GAP inquiry returns both; today connect is by MAC).
  4. **Incoming-pair approval popup.** When the phone requests pairing, show an
     accept/reject prompt on the carsh screen (SSP confirmation) instead of silently
     accepting — wire BTstack's pairing/IO-cap callbacks to a UI dialog.
  5. **Now-playing display.** Song title, artist/album, and album art via AVRCP
     metadata + cover-art (the `NowPlayingAssembler` infra already exists; surface it
     on the Now-Playing screen for the BT source).
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
