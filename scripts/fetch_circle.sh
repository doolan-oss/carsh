#!/usr/bin/env bash
# Fetch + pin circle-stdlib for the device build. carsh's core uses the C++ STL,
# which plain Circle (-nostdinc) does NOT provide — circle-stdlib adds newlib +
# libstdc++ (STDLIB_SUPPORT=4) and bundles Circle as a submodule. It is large and
# is a build dependency, not carsh source, so it lives under
# third_party/circle-stdlib and is gitignored — this script reproduces it.
#
# Pinned commit bundles Circle "Step51".
set -euo pipefail

CSTDLIB_COMMIT="a4fbed9b369e8285e4a12b2bb0588511210b83a6"
CSTDLIB_REPO="https://github.com/smuehlst/circle-stdlib.git"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/circle-stdlib"

if [ -d "$DEST/.git" ]; then
  echo "circle-stdlib already present at $DEST ($(git -C "$DEST" rev-parse --short HEAD))"
  exit 0
fi

echo "Cloning circle-stdlib into $DEST (with submodules: Circle, newlib, mbedTLS) ..."
git clone "$CSTDLIB_REPO" "$DEST"
git -C "$DEST" checkout --quiet "$CSTDLIB_COMMIT"
git -C "$DEST" submodule update --init --recursive --depth 1
echo "Done: circle-stdlib @ $(git -C "$DEST" rev-parse --short HEAD), Circle @ $(git -C "$DEST/libs/circle" describe --tags 2>/dev/null || echo '?')"
echo
echo "Next:"
echo "  cd third_party/circle-stdlib"
echo "  ./configure -r 4 --prefix aarch64-none-elf-"
echo "  make -j\$(nproc)        # builds newlib + Circle (a few minutes)"
