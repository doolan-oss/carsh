# Notices and third-party components

carsh is licensed under the **GNU General Public License v3.0 or later**
(SPDX: `GPL-3.0-or-later`). See [`LICENSE`](LICENSE) for the full text.

The device image statically links Circle (GPLv3), and parts of the Android Auto
protocol core are derived from GPLv3 projects (see below). Distributed builds are
therefore GPLv3 as a whole.

## Vendored in this repository

| Component | Path | Licence |
|---|---|---|
| dr_libs (dr_flac, dr_mp3, dr_wav), David Reid | `third_party/dr_libs/` | Public domain (Unlicense) or MIT-0 (see headers) |
| stb_image / stb_image_write, Sean Barrett | `third_party/stb/` | Public domain or MIT (see headers) |
| TweetNaCl, Bernstein et al. | `third_party/tweetnacl/` | Public domain |
| SHA-256, Brad Conte | `third_party/sha256/` | Public domain |
| font8x8_basic, Daniel Hepper (from Marcel Sondaar / IBM BIOS) | `third_party/font/` | Public domain |
| Realtek RTL8761BU Bluetooth firmware | `device/sdcard/firmware/rtlbt/` | Realtek redistributable firmware licence (linux-firmware `LICENCE.rtlwifi_firmware.txt`), not GPL. See the README in that folder |

## Fetched at build time (not in this repository)

| Component | Fetched by | Licence |
|---|---|---|
| circle-stdlib (Circle, newlib, mbedTLS) | `scripts/fetch_circle.sh` | Circle: GPLv3. newlib: BSD-style (various). mbedTLS: Apache-2.0 |
| Android Auto head-unit certificate and key, from [aasdk](https://github.com/f1xpl/aasdk) | `scripts/fetch_aa_cert.sh` | GPLv3 (aasdk) |
| OpenH264 (Cisco) | `scripts/fetch_openh264.sh` | BSD-2-Clause |
| BTstack (BlueKitchen) | `scripts/fetch_btstack.sh` | BlueKitchen licence: **non-commercial use only** (see below) |

`scripts/patches/` contains patches against circle-stdlib and Circle. They are
derivative works of Circle and are distributed under GPLv3.

## Derived code

The Android Auto protocol core in `src/aa/` and `device/AaCryptoMbedTls.*` was
written with reference to, and partly ported from, the GPLv3
[aasdk](https://github.com/f1xpl/aasdk) project and the author's own `head_unit`
ESP32 project. It is distributed under GPLv3 or later.

## BTstack and the `CARSH_HAVE_BTSTACK=1` build

BTstack's licence allows use only for non-commercial purposes. That restriction is
incompatible with the GPL. The default build links a Bluetooth stub and does not
include BTstack. An image built with `make CARSH_HAVE_BTSTACK=1` combines GPLv3
code (Circle, carsh) with BTstack. You may build and use it yourself, but you
should not distribute that image.

## Trademarks

Android Auto is a trademark of Google LLC. carsh is not affiliated with or endorsed
by Google, JVC-Kenwood, Realtek or MINIX.
