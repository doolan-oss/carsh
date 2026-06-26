#!/usr/bin/env bash
# Fetch + pin the Circle framework for the device build. Circle is large and is
# a build dependency, not carsh source, so it lives under third_party/circle and
# is gitignored — this script reproduces the exact pinned version instead.
#
# Pinned to Circle "Step51" (the carsh device build targets this release).
set -euo pipefail

CIRCLE_TAG="Step51"
CIRCLE_REPO="https://github.com/rsta2/circle.git"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/circle"

if [ -d "$DEST/.git" ]; then
  echo "Circle already present at $DEST"
  git -C "$DEST" describe --tags 2>/dev/null || true
  exit 0
fi

echo "Cloning Circle $CIRCLE_TAG into $DEST ..."
git clone --branch "$CIRCLE_TAG" --depth 1 "$CIRCLE_REPO" "$DEST"
echo "Done: $(git -C "$DEST" log --oneline -1)"
