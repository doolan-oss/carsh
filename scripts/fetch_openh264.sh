#!/usr/bin/env bash
# Fetch + pin Cisco openh264 for the Android Auto software H.264 decoder (AA_PLAN
# rung 5). openh264 is BSD-2 and ships Constrained-Baseline decode + NEON, which is
# exactly what AA video needs. It lives under third_party/openh264 (gitignored) —
# this script reproduces the pinned version. The host build links the distro
# libopenh264 (see CMakeLists); the device build links an aarch64 bare-metal static
# lib built from this tree (see "Next" below + device/Makefile CARSH_HAVE_OPENH264).
set -euo pipefail

# Pin a known-good release matching the host package line (2.4.x). Update
# deliberately and re-validate the decode round-trip test.
OPENH264_TAG="${1:-v2.4.1}"
OPENH264_REPO="https://github.com/cisco/openh264.git"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/openh264"

if [ -d "$DEST/.git" ]; then
  echo "openh264 already present at $DEST"
  git -C "$DEST" describe --tags 2>/dev/null || true
  exit 0
fi

echo "Cloning openh264 $OPENH264_TAG into $DEST ..."
git clone --branch "$OPENH264_TAG" --depth 1 "$OPENH264_REPO" "$DEST"
echo "Done: $(git -C "$DEST" log --oneline -1)"
echo
echo "Next (device):"
echo "  - Cross-build the decoder-only aarch64 bare-metal static lib:"
echo "      bash scripts/build_openh264_baremetal.sh   (needs the cross toolchain)"
echo "    It compiles the decoder + common sources (+ AArch64 NEON) with Circle's ABI,"
echo "    pulls newlib pthread/sem headers from scripts/openh264_baremetal_include, and"
echo "    links a no-op threading shim (openh264_baremetal_shim.c). -> libopenh264.a."
echo "  - Build the image with: cd device && make CARSH_HAVE_OPENH264=1"
echo "  - [hw] bench: validate decode on the A72 + measure FPS (feed() on its own core)."
