# Hardware TODO — bench validation backlog

Each item is a task that **cannot be meaningfully validated without the physical
board/peripheral**. Built behind its interface and host-tested where possible;
this is the prioritized list for the first hardware session. Format:

`Hn — <module/file>` · **Validate:** <exact step> · **Stub now:** <current behavior>

---

### H0 — Circle image build + board boot  [MOSTLY DONE]
- **Module:** build infra, `CKernel`.
- **Done:** ARM `aarch64-none-elf` 14.2 toolchain + `qemu-system-aarch64`
  installed; **circle-stdlib** pinned (`scripts/fetch_circle.sh`) and built
  (`STDLIB_SUPPORT=4`: newlib + libstdc++ — required because the core uses the
  C++ STL). `device/` has `CKernel` + the `CircleRenderTarget` framebuffer
  backend; **`kernel8-rpi4.img` builds** and **boots under QEMU raspi3b without
  faulting**. This retired the biggest risk: the STL core runs on the target ABI.
- **Bench validate:** Flash `kernel8-rpi4.img` (as `kernel8.img`) + Pi firmware +
  `config.txt` (`enable_uart=1`) to a FAT32 SD; confirm the boot log on the PL011
  UART (115200) and the carsh UI on the panel. (QEMU here can't surface serial
  *text* headless — see device/README; the board is authoritative.)
- **Remaining (next device milestones, not blocked):** wire the rest of the
  Circle backends — `CFATFileSystem`→`Vfs`, `CI2SSoundBaseDevice`→`ISoundDevice`,
  `CRPiTouchScreen`→`ITouch`, SD boot-slot→`IFlashTarget` — plus the decode +
  scheduler tasks and the BTstack transport bridge.

### H1 — DSI panel / framebuffer bring-up (Risk R1)
- **Module:** `CKernel`, IRenderTarget Circle backend.
- **Validate:** On the real Pi 4 + official 7" DSI panel, confirm VideoCore brings
  up a framebuffer (pin firmware + `config.txt`); clear to bg color, blit one glyph.
- **Stub now:** UI renders to PngRenderTarget on host; no panel init code yet.

### H2 — Touch reports points (Risk R2, resolved-by-research)
- **Module:** Touch Circle backend (`CRPiTouchScreen`).
- **Validate:** Confirm `CRPiTouchScreen` reports points on the panel; poll >60 Hz;
  honor `count==99` "updating" sentinel. Just a sanity check, not bus bring-up.
- **Stub now:** SyntheticTouch injects scripted events on host.

### H3 — I2S DAC sine + clocking (PCM5122 / HiFiBerry DAC+)
- **Module:** AudioEngine Circle `CI2SSoundBaseDevice` backend; PCM5122 I2C config.
- **Validate:** Play a synthesized sine out the DAC; verify reclock at
  44.1/48/96/192 kHz and bit-perfect passthrough. Also PWM-jack bring-up first.
- **Stub now:** AudioEngine drives WavSinkDevice (host pump); output diffable as WAV.

### H4 — Real-time / underrun behavior under load (Risk R8)
- **Module:** AudioEngine, decode task, scheduler yield discipline.
- **Validate:** Confirm no underruns with crossfade + indexer + UI + BT live;
  measure worst-case cooperative slice; consider `CMultiCoreSupport` for decode.
- **Stub now:** Host pump is synchronous/deterministic; underrun counter exists but
  cannot exercise real DMA timing.

### H5 — USB host + FAT across drives/hubs (Risk R5)
- **Module:** Vfs Circle backend (`CUSBHCIDevice` + `CFATFileSystem`).
- **Validate:** Mount + stream from a matrix of USB drives/hubs; hot-unplug mid-read.
- **Stub now:** HostVfs over a real directory; FAT32-only mandate documented.

### H6 — Bluetooth: USB ACL path + firmware blob + bring-up (Risks R3/R4/R12)
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
