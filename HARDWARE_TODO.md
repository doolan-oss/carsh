# Hardware TODO — bench validation backlog

Each item is a task that **cannot be meaningfully validated without the physical
board/peripheral**. Built behind its interface and host-tested where possible;
this is the prioritized list for the first hardware session. Format:

`Hn — <module/file>` · **Validate:** <exact step> · **Stub now:** <current behavior>

---

### H0 — Circle image build + QEMU/board boot
- **Module:** build infra, `CKernel`.
- **Status update:** the AArch64 toolchain (`aarch64-linux-gnu-gcc`) and
  `qemu-system-aarch64` were **not** preinstalled but have since been installed
  here (via apt). What remains absent is the **Circle source tree + its Makefile
  build** — so no `kernel8.img` is produced yet, and the HAL interfaces have no
  Circle backends wired.
- **Validate:** Clone rsta2/circle, build it with the toolchain, implement the
  Circle backends behind the HAL interfaces (CBcmFrameBuffer→IRenderTarget,
  CI2SSoundBaseDevice→ISoundDevice, CFATFileSystem→Vfs, CRPiTouchScreen→ITouch),
  link the carsh core, and boot under `qemu raspi4b` (then real Pi 4) for
  boot/scheduler/FB/SD-FAT smoke.
- **Stub now:** All carsh logic is host-compiled with g++ 15 against the HAL
  interfaces and host backends. Do this first at the bench — it unblocks QEMU-
  level checks for everything below short of the analog peripherals.

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

### H6 — Bluetooth: firmware blob + transport bridge (Risks R3/R4/R12)
- **Module:** BTstack↔Circle HCI transport, A2DP sink, AVRCP CT/TG.
- **Validate:** Load CYW43455 .hcd; HCI reset/read-local-version probe; pair a phone;
  A2DP SBC/AAC → ring; AVRCP metadata + control both ways; clock-drift adaptive SRC.
- **Stub now:** BtSource and AVRCP are interface stubs only (Phase 3 = logic stubs).

### H7 — WiFi STA + OTA flash/rollback (Risk R11)
- **Module:** NetSvc (CYW43455 STA), OTA flash writer, chainloader A/B.
- **Validate:** Known-SSID detect; HTTPS manifest fetch; write inactive slot; forced-
  bad-boot rollback survives. The *flash write* and *chainloader jump* are HW-only.
- **Stub now:** OTA package parse/verify + A/B state-machine decision logic are
  host-tested; the actual slot write + reboot are stubbed behind an interface.
