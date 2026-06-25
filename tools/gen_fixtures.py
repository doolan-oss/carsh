#!/usr/bin/env python3
"""Generate deterministic audio fixtures for host decoder/library tests.

Writes a known sine WAV, then (if `flac`/`lame` are on PATH) lossless FLAC and
lossy MP3 encodings of it, plus a small tagged-file tree for the indexer. The
encoders are best-effort: missing tools just skip those fixtures, and the
corresponding tests report "skipped" rather than failing the suite.

Determinism matters — golden tests diff against these, so the signal is fixed.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

RATE = 44100
CHANNELS = 2
FREQ_L = 440.0
FREQ_R = 660.0
AMP = 0.5
DUR_S = 0.5


def write_sine_wav(path):
    n = int(RATE * DUR_S)
    with wave.open(path, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(2)  # 16-bit
        w.setframerate(RATE)
        frames = bytearray()
        for i in range(n):
            t = i / RATE
            l = int(AMP * math.sin(2 * math.pi * FREQ_L * t) * 32767)
            r = int(AMP * math.sin(2 * math.pi * FREQ_R * t) * 32767)
            frames += struct.pack("<hh", l, r)
        w.writeframes(bytes(frames))
    return n


def try_run(cmd):
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
    os.makedirs(out, exist_ok=True)
    wav = os.path.join(out, "sine.wav")
    n = write_sine_wav(wav)
    print(f"wrote {wav} ({n} frames)")

    if shutil.which("flac"):
        flac = os.path.join(out, "sine.flac")
        if try_run(["flac", "--totally-silent", "-f", "-o", flac, wav]):
            print(f"wrote {flac}")
    else:
        print("flac not found — skipping FLAC fixture")

    if shutil.which("lame"):
        mp3 = os.path.join(out, "sine.mp3")
        if try_run(["lame", "--quiet", "-h", wav, mp3]):
            print(f"wrote {mp3}")
    else:
        print("lame not found — skipping MP3 fixture")

    # Small library tree: a couple of "albums" of WAVs for the indexer. Tags are
    # added later by the tag-writer fixtures; for now filenames carry structure.
    lib = os.path.join(out, "library")
    for album in ("ArtistA - Album1", "ArtistB - Album2"):
        d = os.path.join(lib, album)
        os.makedirs(d, exist_ok=True)
        for trk in range(1, 3):
            write_sine_wav(os.path.join(d, f"{trk:02d} - Track{trk}.wav"))
    print(f"wrote library tree under {lib}")


if __name__ == "__main__":
    main()
