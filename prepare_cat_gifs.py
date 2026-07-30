#!/usr/bin/env python3
"""Download and resize cat GIFs into a board-friendly library for the CYD.

Fetches random cat GIFs from Cataas (cataas.com, no API key), resizes/optimizes
each to fit the 320x240 CYD screen, and writes them to an output folder you copy
to the SD card as /cats/. Page 6 of the firmware plays them at random, endlessly.

Prefers `gifsicle` (best GIF optimizer); falls back to ImageMagick (`magick`/
`convert`) if gifsicle isn't installed. For smoothest playback on the ESP32:
    brew install gifsicle

Standalone, re-runnable (continues the cat_NNN numbering so you can grow the
library over several runs):
    python3 prepare_cat_gifs.py                 # 40 cats into ./cats
    python3 prepare_cat_gifs.py --count 100     # a bigger library
    python3 prepare_cat_gifs.py --colors 128    # smaller/lighter for the board

Then: COPYFILE_DISABLE=1 cp -r cats /Volumes/<SD>/cats
"""

import argparse
import hashlib
import http.client
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

CATAAS_GIF_URL = "https://cataas.com/cat/gif"
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) prepare_cat_gifs/1.0"
GIF_MAGIC = (b"GIF87a", b"GIF89a")

# Warn above this per-file size: big GIFs strain the ESP32's LZW buffers and SD
# read cadence, and make page 6 stutter. Not fatal — just flagged.
BIG_FILE_WARN = 600 * 1024


# ── TOOLING ────────────────────────────────────────────────
def pick_resizer():
    """Return ('gifsicle'|'imagemagick', binary_path) or exit with guidance."""
    if shutil.which("gifsicle"):
        return "gifsicle", "gifsicle"
    for im in ("magick", "convert"):
        if shutil.which(im):
            return "imagemagick", im
    sys.exit(
        "No GIF resizer found. Install one of:\n"
        "  brew install gifsicle      (recommended — best GIF optimizer)\n"
        "  brew install imagemagick"
    )


def count_frames(src):
    """Number of frames in a GIF, via ImageMagick identify if present, else
    gifsicle --info. Returns 0 if it can't tell (callers then skip thinning)."""
    im = shutil.which("magick") or shutil.which("convert")
    if im:
        try:
            out = subprocess.run([im, "identify", "-format", "%n\n", src] if "magick" in im
                                 else [im, "-format", "%n\n", src],
                                 check=True, capture_output=True, timeout=60)
            return int(out.stdout.split()[0])
        except Exception:
            pass
    if shutil.which("gifsicle"):
        try:
            out = subprocess.run(["gifsicle", "--info", src],
                                 check=True, capture_output=True, timeout=60)
            m = re.search(rb"(\d+) images?", out.stdout)
            if m:
                return int(m.group(1))
        except Exception:
            pass
    return 0


def even_indices(n, cap):
    """`cap` evenly-spaced frame indices out of n (keeps motion, drops density)."""
    if n <= cap:
        return list(range(n))
    step = n / cap
    return sorted({int(i * step) for i in range(cap)})


def resize_gif(resizer, binary, src, dst, w, h, colors, max_frames, lossy):
    """Fit src within w x h (preserve aspect, never upscale), thin to at most
    max_frames, quantize to `colors`, and optimize. Returns True on success."""
    n = count_frames(src)
    idx = even_indices(n, max_frames) if n else None

    try:
        if resizer == "gifsicle":
            cmd = [binary, "--unoptimize", src]
            if idx and len(idx) < n:
                cmd += [f"#{i}" for i in idx]
            cmd += ["--resize-fit", f"{w}x{h}", "--colors", str(colors)]
            if lossy:
                cmd += [f"--lossy={lossy}"]
            cmd += ["-O3", "-o", dst]
            subprocess.run(cmd, check=True, capture_output=True, timeout=180)
        else:
            # ImageMagick: coalesce to full frames first (so frame selection and
            # resize don't corrupt delta-encoded frames), then thin + resize.
            frames = src
            tmp_co = None
            try:
                if idx and len(idx) < n:
                    tmp_co = src + ".co.gif"
                    subprocess.run([binary, src, "-coalesce", tmp_co],
                                   check=True, capture_output=True, timeout=180)
                    sel = ",".join(str(i) for i in idx)
                    frames = f"{tmp_co}[{sel}]"
                cmd = [binary, frames, "-coalesce", "-resize", f"{w}x{h}>",
                       "-fuzz", "3%", "-layers", "Optimize", "-colors", str(colors), dst]
                subprocess.run(cmd, check=True, capture_output=True, timeout=180)
            finally:
                # Must run even if the resize step above raises -- otherwise a
                # failed batch leaves .co.gif temp files behind forever.
                if tmp_co and os.path.exists(tmp_co):
                    os.remove(tmp_co)
        return os.path.exists(dst) and os.path.getsize(dst) > 0
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        stderr = getattr(e, "stderr", b"") or b""
        print(f"    resize failed: {stderr.decode('utf-8', 'replace')[:200]}")
        return False


# ── DOWNLOAD ───────────────────────────────────────────────
def download_gif(timeout=20):
    """Fetch one random cat GIF. Returns bytes, or None on failure / non-GIF."""
    req = urllib.request.Request(CATAAS_GIF_URL, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
    except (urllib.error.URLError, TimeoutError, OSError, http.client.IncompleteRead) as e:
        # IncompleteRead isn't an OSError/URLError subclass, so a truncated
        # response (connection dropped mid-body) would otherwise crash the
        # whole batch instead of just skipping this one download.
        print(f"    download failed: {e}")
        return None
    if not data.startswith(GIF_MAGIC):
        print("    skipped: response was not a GIF")
        return None
    return data


def next_index(outdir):
    """Continue numbering from any cat_NNN.gif already in outdir."""
    hi = 0
    for name in os.listdir(outdir) if os.path.isdir(outdir) else []:
        m = re.match(r"cat_(\d+)\.gif$", name)
        if m:
            hi = max(hi, int(m.group(1)))
    return hi + 1


def existing_hashes(outdir):
    """Hashes of already-saved (resized) files, to skip re-adding duplicates
    across re-runs. Hashes the resized output, so it's approximate but cheap."""
    seen = set()
    if not os.path.isdir(outdir):
        return seen
    for name in os.listdir(outdir):
        if name.endswith(".gif"):
            with open(os.path.join(outdir, name), "rb") as f:
                seen.add(hashlib.md5(f.read()).hexdigest())
    return seen


# ── MAIN ───────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Download + resize cat GIFs for the CYD.")
    ap.add_argument("--count", type=int, default=40, help="how many new GIFs to add (default 40)")
    ap.add_argument("--outdir", default="cats", help="output folder (default ./cats)")
    ap.add_argument("--width", type=int, default=320, help="max width (default 320)")
    ap.add_argument("--height", type=int, default=240, help="max height (default 240)")
    ap.add_argument("--colors", type=int, default=128, help="palette size, 2-256 (default 128)")
    ap.add_argument("--max-frames", type=int, default=40,
                    help="thin GIFs to at most this many frames (default 40)")
    ap.add_argument("--lossy", type=int, default=80,
                    help="gifsicle lossy level, higher=smaller; 0 off (default 80)")
    ap.add_argument("--max-attempts", type=int, default=0,
                    help="cap total download attempts (default: count*4)")
    args = ap.parse_args()

    resizer, binary = pick_resizer()
    print(f"Using {resizer} ({binary}); target {args.width}x{args.height}, {args.colors} colors")

    os.makedirs(args.outdir, exist_ok=True)
    idx = next_index(args.outdir)
    seen = existing_hashes(args.outdir)
    raw_seen = set()  # raw-download hashes, dedupe before spending a resize

    added = 0
    attempts = 0
    max_attempts = args.max_attempts or args.count * 4
    total_bytes = 0

    with tempfile.TemporaryDirectory() as tmp:
        while added < args.count and attempts < max_attempts:
            attempts += 1
            print(f"[{added + 1}/{args.count}] downloading (attempt {attempts})...")
            data = download_gif()
            if data is None:
                time.sleep(1.0)  # brief backoff; cataas rate-limits bursts
                continue

            raw_hash = hashlib.md5(data).hexdigest()
            if raw_hash in raw_seen:
                print("    skipped: duplicate download")
                continue
            raw_seen.add(raw_hash)

            raw_path = os.path.join(tmp, f"raw_{attempts}.gif")
            with open(raw_path, "wb") as f:
                f.write(data)

            dst = os.path.join(args.outdir, f"cat_{idx:03d}.gif")
            if not resize_gif(resizer, binary, raw_path, dst,
                              args.width, args.height, args.colors,
                              args.max_frames, args.lossy):
                continue

            with open(dst, "rb") as f:
                out_hash = hashlib.md5(f.read()).hexdigest()
            if out_hash in seen:
                os.remove(dst)
                print("    skipped: duplicate after resize")
                continue
            seen.add(out_hash)

            size = os.path.getsize(dst)
            total_bytes += size
            flag = "  <-- large, may stutter" if size > BIG_FILE_WARN else ""
            print(f"    saved {os.path.basename(dst)}  ({size // 1024} KB){flag}")
            idx += 1
            added += 1
            time.sleep(0.3)  # be polite to cataas

    print("\n" + "=" * 48)
    print(f"Added {added} cat GIF(s) to {args.outdir}/  ({total_bytes // 1024} KB total)")
    if added < args.count:
        print(f"(wanted {args.count}; stopped after {attempts} attempts — "
              "cataas may be rate-limiting. Re-run to add more.)")
    print(f"Copy to the SD card:  COPYFILE_DISABLE=1 cp -r {args.outdir} /Volumes/<SD>/cats")
    print("  (COPYFILE_DISABLE=1 stops macOS from writing '._*.gif' AppleDouble junk")


if __name__ == "__main__":
    main()
