# Open Questions — research-blocked / decisions needed

Items where a needed fact was not confidently resolvable, or a product decision is
genuinely the owner's. Each: what was needed, what was found, why unresolved, the
decision required, and the safest assumption taken to keep the build green.

PLAN.md §8 already resolved licensing (GPL), exFAT (skip, FAT32), DAC/DSD (PCM5122,
no DSD), and the OTA scheme. Remaining items below are carried from §8 plus anything
surfaced during host build.

---

### Q1 — Library scale target (PLAN.md §8.9)
- **Needed:** Max track count to size the in-RAM index strategy.
- **Found:** Plan floats 5k–200k; §4.1 says in-RAM index holds at 10k (~few MB).
- **Unresolved:** Owner decision; affects whether index must page to SD.
- **Assumption taken:** Designed the DB + indexes to hold **≤50k tracks** fully in
  RAM. Record sizes are fixed (128 B) so 50k ≈ 6.4 MB of records + a few MB of
  indexes — comfortable. Revisit if the target exceeds ~100k.

### Q2 — WiFi credential provisioning (PLAN.md §8.6)
- **Needed:** Pre-baked config file vs setup screen for SSID/secret.
- **Unresolved:** Owner UX decision; affects Settings screen + persistence schema.
- **Assumption taken:** OTA/profile code reads WiFi creds from a `/carsh/net.cfg`
  file (key=value). A Settings editor can be layered later; the file is the source
  of truth now.

### Q3 — "Driving mode" trigger source (PLAN.md §8.8)
- **Needed:** Eventual signal (GPS speed / CAN) for driving-mode.
- **Unresolved:** Out of scope; interface shape only.
- **Assumption taken:** Stub `IDrivingSignal { bool isDriving(); float speedKmh(); }`
  to be wired later; UI consumes the boolean only.

### Q4 — BT cover art (BIP/OBEX) must-have? (PLAN.md §8.4)
- **Needed:** Whether AVRCP cover art is required for BT sources.
- **Unresolved:** Owner decision; plan recommends best-effort.
- **Assumption taken:** Text metadata required; BT cover art best-effort (generic
  placeholder art when BIP is unavailable). No host impact yet (BT is stubbed).

### Q5 — OTA private-key custody + HTTPS endpoint (PLAN.md §8.5)
- **Needed:** Who holds the offline signing key; where the manifest is hosted.
- **Unresolved:** Genuinely the owner's call.
- **Assumption taken:** Ship **USB-stick OTA only** (no endpoint needed). The image
  embeds a compiled-in Ed25519 public key (placeholder test key in the host build);
  verify→stage→A/B→try→commit path is identical for network later.

### Q6 — Crash-log privacy/consent model (PLAN.md §8.10)
- **Needed:** Consent model + what is captured.
- **Unresolved:** Owner decision.
- **Assumption taken:** Crash-log capture disabled by default; when enabled, capture
  only fault PC/registers/stack + version — no library contents, no PII. AES-GCM
  encrypted. Logic deferred (low priority); flagged here.
