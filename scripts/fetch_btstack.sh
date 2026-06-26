#!/usr/bin/env bash
# Fetch + pin BTstack for the Bluetooth (A2DP sink + AVRCP) device build.
# BTstack is large and dual-licensed (free for open-source/non-commercial), so it
# lives under third_party/btstack and is gitignored — this script reproduces the
# pinned version. After fetching, define CARSH_HAVE_BTSTACK and complete
# device/BtStackGlue.cpp against port/raspi + the A2DP/AVRCP examples.
set -euo pipefail

# Pin a known-good tag (update deliberately). v1.6.1 is a recent stable release.
BTSTACK_TAG="${1:-v1.6.1}"
BTSTACK_REPO="https://github.com/bluekitchen/btstack.git"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/btstack"

if [ -d "$DEST/.git" ]; then
  echo "BTstack already present at $DEST"
  git -C "$DEST" describe --tags 2>/dev/null || true
  exit 0
fi

echo "Cloning BTstack $BTSTACK_TAG into $DEST ..."
git clone --branch "$BTSTACK_TAG" --depth 1 "$BTSTACK_REPO" "$DEST"
echo "Done: $(git -C "$DEST" log --oneline -1)"
echo
echo "Next:"
echo "  - Reference port/raspi (UART transport + chipset/bcm firmware download)"
echo "  - Pin the BCM/CYW .hcd firmware on the SD boot partition"
echo "  - Build BTstack src/ + classic/{a2dp_sink,avrcp}.c into the device image"
echo "  - Define CARSH_HAVE_BTSTACK and complete device/BtStackGlue.cpp"
