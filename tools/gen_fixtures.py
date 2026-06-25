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

    if shutil.which("opusenc"):
        opus = os.path.join(out, "sine.opus")
        if try_run(["opusenc", "--quiet", wav, opus]):
            print(f"wrote {opus}")
    else:
        print("opusenc not found — skipping Opus fixture")

    if shutil.which("faac"):
        aac = os.path.join(out, "sine.aac")  # raw ADTS transport (no -w/MP4 wrap)
        if try_run(["faac", "-o", aac, wav]):
            print(f"wrote {aac}")
    else:
        print("faac not found — skipping AAC fixture")

    # Tagged single-track fixtures for the tag parsers. Known values so tests
    # can assert exact title/artist/album/year/track across MP3/FLAC/Opus.
    T = dict(title="Night Drive", artist="Carsh Test", album="Demos",
             year="2023", track="3")
    if shutil.which("lame"):
        m = os.path.join(out, "tagged.mp3")
        try_run(["lame", "--quiet", "--tt", T["title"], "--ta", T["artist"],
                 "--tl", T["album"], "--ty", T["year"], "--tn", T["track"],
                 wav, m])
        print(f"wrote {m}")
    if shutil.which("flac"):
        f = os.path.join(out, "tagged.flac")
        try_run(["flac", "--totally-silent", "-f", "-o", f,
                 f"--tag=TITLE={T['title']}", f"--tag=ARTIST={T['artist']}",
                 f"--tag=ALBUM={T['album']}", f"--tag=DATE={T['year']}",
                 f"--tag=TRACKNUMBER={T['track']}", wav])
        print(f"wrote {f}")
    if shutil.which("opusenc"):
        o = os.path.join(out, "tagged.opus")
        try_run(["opusenc", "--quiet", "--title", T["title"],
                 "--artist", T["artist"], "--album", T["album"],
                 "--comment", f"DATE={T['year']}",
                 "--comment", f"TRACKNUMBER={T['track']}", wav, o])
        print(f"wrote {o}")

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
