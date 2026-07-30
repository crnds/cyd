#!/usr/bin/env python3
"""Download and manage a large library of cat GIFs from GIPHY.

This script queries GIPHY for multiple categories of cat GIFs (cartoons, memes,
kawaii, etc.), downloads the raw (original size) GIFs into a local repository
(e.g., `cats_raw/`) until a target size (default: 2.5 GB) is reached, and then
optionally resizes a subset of them (default: up to 120 total files) using the CYD
resizer logic to fit the board's requirements.

It respects GIPHY's rate limits by using polite delays and handling 429 errors
with exponential backoff, and it is fully resumable across multiple runs.
"""

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

GIF_MAGIC = (b"GIF87a", b"GIF89a")
GIPHY_MAX_OFFSET = 4999  # GIPHY search caps offset+limit around 5000 results total


def load_giphy_api_key():
    """GIPHY_API_KEY env var, then secrets.local.json (repo root, gitignored,
    same convention as usage_server.py's WEATHER_API_KEY). No hardcoded
    fallback -- a key must never be committed to source."""
    env_key = os.environ.get("GIPHY_API_KEY")
    if env_key:
        return env_key
    secrets_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "secrets.local.json")
    if not os.path.exists(secrets_path):
        return None
    try:
        with open(secrets_path, "r") as f:
            return (json.load(f) or {}).get("GIPHY_API_KEY")
    except Exception as e:
        print(f"[warn] failed reading secrets.local.json: {type(e).__name__}: {e}")
        return None


DEFAULT_API_KEY = load_giphy_api_key()

# Attempt to import resizing helpers from prepare_cat_gifs.py
try:
    from prepare_cat_gifs import (
        pick_resizer,
        resize_gif,
        next_index,
        existing_hashes
    )
    HAS_RESIZER = True
except ImportError:
    HAS_RESIZER = False

# Fallback implementations in case prepare_cat_gifs.py is moved/missing
if not HAS_RESIZER:
    def pick_resizer():
        if shutil.which("gifsicle"):
            return "gifsicle", "gifsicle"
        for im in ("magick", "convert"):
            if shutil.which(im):
                return "imagemagick", im
        return None, None

    def next_index(outdir):
        hi = 0
        for name in os.listdir(outdir) if os.path.isdir(outdir) else []:
            m = re.match(r"cat_(\d+)\.gif$", name)
            if m:
                hi = max(hi, int(m.group(1)))
        return hi + 1

    def existing_hashes(outdir):
        seen = set()
        if not os.path.isdir(outdir):
            return seen
        for name in os.listdir(outdir):
            if name.endswith(".gif"):
                try:
                    with open(os.path.join(outdir, name), "rb") as f:
                        seen.add(hashlib.md5(f.read()).hexdigest())
                except OSError:
                    pass
        return seen

    def resize_gif(*args, **kwargs):
        print("    Warning: Resizing is unavailable. Please check prepare_cat_gifs.py.")
        return False


def get_existing_raw_gifs(raw_dir):
    """Scan raw_dir for downloaded GIPHY files.
    
    Returns:
        (total_bytes, seen_ids)
    """
    total_bytes = 0
    seen_ids = set()
    if not os.path.isdir(raw_dir):
        return 0, seen_ids

    for name in os.listdir(raw_dir):
        if name.startswith("giphy_") and name.endswith(".gif"):
            path = os.path.join(raw_dir, name)
            try:
                size = os.path.getsize(path)
                if size == 0:
                    continue
                # A truncated download (e.g. killed mid-write on a previous
                # run) can leave a non-empty file that isn't a real GIF --
                # size alone would count it toward the target and toward
                # seen_ids, permanently skipping a re-download of that id.
                with open(path, "rb") as f:
                    header = f.read(6)
                if not header.startswith(GIF_MAGIC):
                    continue
                total_bytes += size
                gif_id = name[6:-4]  # Extract ID from 'giphy_<id>.gif'
                seen_ids.add(gif_id)
            except OSError:
                pass
    return total_bytes, seen_ids


def query_giphy_search(api_key, query, limit=50, offset=0, timeout=20):
    """Query GIPHY API search endpoint for a specific query and offset.
    
    Returns:
        (results_list, rate_limit_remaining, err_code)
    """
    safe_query = urllib.parse.quote(query)
    url = f"https://api.giphy.com/v1/gifs/search?api_key={api_key}&q={safe_query}&limit={limit}&offset={offset}&rating=pg-13"
    
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) pull_giphy_cats/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            # Try to read rate limit remaining header
            rate_remaining = resp.headers.get("X-RateLimit-Remaining")
            if rate_remaining is not None:
                try:
                    rate_remaining = int(rate_remaining)
                except ValueError:
                    rate_remaining = None
            
            data = json.loads(resp.read().decode("utf-8"))
            return data.get("data", []), rate_remaining, 200
    except urllib.error.HTTPError as e:
        print(f"\n[GIPHY API HTTP Error] Status Code: {e.code}")
        # Return code so we can handle 429 specifically
        return [], None, e.code
    except Exception as e:
        print(f"\n[GIPHY API Network/Parse Error] {e}")
        return [], None, 500


def download_gif(url, dest_path, timeout=30):
    """Download a single GIF file from GIPHY CDN."""
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
    except Exception as e:
        print(f"    Download failed: {e}")
        return None

    if not data.startswith(GIF_MAGIC):
        print("    Skipped: Downloaded content did not start with GIF magic header")
        return None

    try:
        with open(dest_path, "wb") as f:
            f.write(data)
        return len(data)
    except OSError as e:
        print(f"    Failed to write file {dest_path}: {e}")
        return None


def format_size(size_bytes):
    """Format bytes to human readable format."""
    for unit in ['B', 'KB', 'MB', 'GB']:
        if size_bytes < 1024.0:
            return f"{size_bytes:.2f} {unit}"
        size_bytes /= 1024.0
    return f"{size_bytes:.2f} TB"


def main():
    ap = argparse.ArgumentParser(description="Download and resize cat GIFs from GIPHY.")
    ap.add_argument("--api-key", default=DEFAULT_API_KEY,
                     help="GIPHY API key. Prefer the GIPHY_API_KEY env var or "
                          "secrets.local.json's GIPHY_API_KEY field over this flag -- "
                          "a CLI arg is visible to any local user via `ps`/shell history.")
    ap.add_argument("--target-gb", type=float, default=2.5, help="Target library size in GB (default: 2.5)")
    ap.add_argument("--raw-dir", default="cats_raw", help="Directory for raw GIPHY GIFs (default: cats_raw)")
    ap.add_argument("--out-dir", default="cats", help="Board-ready resized folder (default: ./cats)")
    ap.add_argument("--skip-resize", action="store_true", help="Skip resizing/optimizing for the CYD board")
    ap.add_argument("--resize-count", type=int, default=120, help="Max number of GIFs to have resized for the board (default: 120)")
    
    # Resizing configurations (passed to prepare_cat_gifs resizer)
    ap.add_argument("--width", type=int, default=320, help="CYD width (default: 320)")
    ap.add_argument("--height", type=int, default=240, help="CYD height (default: 240)")
    ap.add_argument("--colors", type=int, default=128, help="CYD palette colors (default: 128)")
    ap.add_argument("--max-frames", type=int, default=40, help="CYD max frames (default: 40)")
    ap.add_argument("--lossy", type=int, default=80, help="Gifsicle lossy optimization (default: 80)")
    
    # Rate limiting configuration
    ap.add_argument("--api-sleep", type=float, default=2.0, help="Pause (seconds) between API queries (default: 2.0)")
    ap.add_argument("--dl-sleep", type=float, default=0.4, help="Pause (seconds) between GIF downloads (default: 0.4)")
    ap.add_argument("--queries", default="cartoon cat,kawaii cat,pusheen,cat meme,funny cat,cat animation,simons cat,garfield,grumpy cat,anime cat,pixel cat,cute kitten,nyan cat,chibi cat,cat play,cat fail,crying cat,space cat,sleeping cat,cat wiggle",
                    help="Comma-separated search terms to query from GIPHY")
    args = ap.parse_args()

    if not args.api_key:
        print("Error: no GIPHY API key found. Set the GIPHY_API_KEY env var, add "
              "\"GIPHY_API_KEY\" to secrets.local.json, or pass --api-key.")
        sys.exit(1)

    # Parse queries list
    queries = [q.strip() for q in args.queries.split(",") if q.strip()]
    if not queries:
        print("Error: No search queries provided.")
        sys.exit(1)

    target_bytes = int(args.target_gb * 1024 * 1024 * 1024)
    os.makedirs(args.raw_dir, exist_ok=True)

    print("=" * 60)
    print("                GIPHY CAT GIF DOWNLOADER")
    print("=" * 60)
    print(f"Target Library Size : {args.target_gb} GB ({target_bytes:,} bytes)")
    print(f"Raw Storage Dir     : {args.raw_dir}/")
    print(f"Resized Output Dir  : {args.out_dir}/")
    print(f"Queries (Round Robin): {len(queries)} categories")
    print("-" * 60)

    # Scan existing files
    total_bytes, seen_ids = get_existing_raw_gifs(args.raw_dir)
    print(f"Found {len(seen_ids)} existing GIPHY GIFs in '{args.raw_dir}/'")
    print(f"Current library size: {format_size(total_bytes)} / {args.target_gb} GB")
    
    if total_bytes >= target_bytes:
        print("\nTarget size already met! Skipping downloads.")
    else:
        # Initialize offset tracking for each query
        query_offsets = {q: 0 for q in queries}
        exhausted_queries = set()
        
        # Round-robin loop
        consecutive_api_errors = 0
        while total_bytes < target_bytes and len(exhausted_queries) < len(queries):
            # Find the queries that are not exhausted
            active_queries = [q for q in queries if q not in exhausted_queries]
            if not active_queries:
                break
                
            for query in active_queries:
                if total_bytes >= target_bytes:
                    break
                
                offset = query_offsets[query]
                if offset > GIPHY_MAX_OFFSET:
                    # Past this, GIPHY returns an error rather than an empty
                    # page, which would otherwise count toward the *global*
                    # consecutive_api_errors abort instead of just retiring
                    # this one query.
                    print(f"    Offset {offset} past GIPHY's ~{GIPHY_MAX_OFFSET} cap. Query '{query}' is exhausted.")
                    exhausted_queries.add(query)
                    continue
                print(f"\n--> Querying GIPHY for '{query}' (offset: {offset})...")

                results, rate_rem, code = query_giphy_search(args.api_key, query, limit=50, offset=offset)
                
                if code == 429:
                    consecutive_api_errors += 1
                    sleep_time = min(60 * (2 ** (consecutive_api_errors - 1)), 600)
                    print(f"    [Rate Limited (429)] Sleeping for {sleep_time} seconds before retrying...")
                    time.sleep(sleep_time)
                    # Don't update offset or count this query as exhausted yet
                    continue
                elif code != 200:
                    consecutive_api_errors += 1
                    if consecutive_api_errors >= 5:
                        print("    Too many consecutive API errors. Stopping downloads.")
                        break
                    time.sleep(5.0)
                    continue
                
                # Success
                consecutive_api_errors = 0
                query_offsets[query] += len(results)
                
                if rate_rem is not None:
                    print(f"    [API Quota] X-RateLimit-Remaining: {rate_rem}")
                
                if not results:
                    print(f"    No results returned. Query '{query}' is exhausted.")
                    exhausted_queries.add(query)
                    continue
                
                if len(results) < 50:
                    print(f"    Returned {len(results)} results (less than page limit). Query '{query}' is exhausted.")
                    exhausted_queries.add(query)

                # Process results and download
                downloaded_in_batch = 0
                for gif in results:
                    if total_bytes >= target_bytes:
                        break
                        
                    gif_id = gif.get("id")
                    if not gif_id:
                        continue
                        
                    if gif_id in seen_ids:
                        continue
                        
                    images = gif.get("images", {})
                    original = images.get("original", {})
                    url = original.get("url")
                    
                    if not url:
                        continue
                        
                    title = gif.get("title", "Cat GIF")
                    dest_file = os.path.join(args.raw_dir, f"giphy_{gif_id}.gif")
                    
                    # Log attempt
                    pct = (total_bytes / target_bytes) * 100
                    print(f"[{format_size(total_bytes)} / {args.target_gb} GB ({pct:.1f}%)] Downloading ID: {gif_id}...")
                    
                    # Sleep slightly to be gentle on CDN
                    time.sleep(args.dl_sleep)
                    
                    file_size = download_gif(url, dest_file)
                    if file_size:
                        total_bytes += file_size
                        seen_ids.add(gif_id)
                        downloaded_in_batch += 1
                        print(f"    Saved: {os.path.basename(dest_file)} ({format_size(file_size)}) - \"{title}\"")
                
                print(f"--> Completed batch for '{query}'. Downloaded {downloaded_in_batch} new GIFs.")
                
                # Sleep between API queries
                if total_bytes < target_bytes:
                    time.sleep(args.api_sleep)
            
            if consecutive_api_errors >= 5:
                break

    print("\n" + "=" * 60)
    print("                   DOWNLOAD SUMMARY")
    print("=" * 60)
    print(f"Final Library Size: {format_size(total_bytes)}")
    print(f"Total Unique GIFs  : {len(seen_ids)}")
    print(f"Location           : {args.raw_dir}/")
    print("-" * 60)

    # Resizing Step (for the CYD display)
    if args.skip_resize:
        print("Skipping resizing step as requested.")
        return

    print("\nStarting resizing sweep for CYD board...")
    os.makedirs(args.out_dir, exist_ok=True)

    # Determine how many GIFs we already have in the out dir
    current_resized_count = 0
    for name in os.listdir(args.out_dir):
        if name.startswith("cat_") and name.endswith(".gif"):
            current_resized_count += 1

    print(f"Current board-ready GIFs in '{args.out_dir}/' : {current_resized_count}")
    print(f"Firmware MAX_CATS Limit                       : {args.resize_count}")
    
    if current_resized_count >= args.resize_count:
        print(f"\n[Warning] Board-ready folder already has {current_resized_count} GIFs.")
        print(f"The firmware array size limit (MAX_CATS) is {args.resize_count}.")
        print("To load a fresh batch, clear or rename the 'cats/' directory first, e.g.:")
        print(f"  mv {args.out_dir} {args.out_dir}_old")
        return

    # Check resizer tools
    resizer, binary = pick_resizer()
    if not resizer:
        print("\n[Warning] No GIF resizer (gifsicle or ImageMagick) found on your system.")
        print("Skipping board resizing step. Please install gifsicle or imagemagick:")
        print("  brew install gifsicle")
        return

    print(f"Using resizer: {resizer} ({binary})")
    
    # We want to fill the remaining slots up to MAX_CATS
    slots_available = args.resize_count - current_resized_count
    print(f"Targeting to generate {slots_available} new resized GIFs for the board.")

    # Find raw GIFs we have downloaded
    raw_files = [
        f for f in os.listdir(args.raw_dir)
        if f.startswith("giphy_") and f.endswith(".gif")
    ]
    
    if not raw_files:
        print("No raw GIFs found to resize!")
        return

    # Find out which resized files are already in out_dir so we don't duplicate them
    resized_hashes = existing_hashes(args.out_dir)
    print(f"Indexed {len(resized_hashes)} hashes from existing board GIFs to prevent duplicates.")

    # Shuffle the raw files so we get a randomized diverse set
    random.shuffle(raw_files)
    
    idx = next_index(args.out_dir)
    resized_added = 0
    resize_attempts = 0
    max_resize_attempts = len(raw_files)
    
    print("\nResizing files...")
    while resized_added < slots_available and resize_attempts < max_resize_attempts:
        raw_name = raw_files[resize_attempts]
        resize_attempts += 1
        
        src_path = os.path.join(args.raw_dir, raw_name)
        dst_name = f"cat_{idx:03d}.gif"
        dst_path = os.path.join(args.out_dir, dst_name)
        
        # Calculate raw file hash to make sure we don't resize it if it's already on the board
        try:
            with open(src_path, "rb") as f:
                raw_hash = hashlib.md5(f.read()).hexdigest()
        except OSError:
            continue
            
        print(f"[{resized_added + 1}/{slots_available}] Resizing {raw_name} -> {dst_name}...")
        
        # Temporary resize to check for duplicates
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_dst = os.path.join(tmp_dir, "temp_resized.gif")
            success = resize_gif(
                resizer, binary, src_path, tmp_dst,
                args.width, args.height, args.colors,
                args.max_frames, args.lossy
            )
            
            if not success:
                print("    Skipped: Resizer failed on this file")
                continue
                
            # Check if this resized file is already in the out dir (by content hash)
            try:
                with open(tmp_dst, "rb") as f:
                    out_hash = hashlib.md5(f.read()).hexdigest()
            except OSError:
                continue
                
            if out_hash in resized_hashes:
                print("    Skipped: Resized content already exists on board (duplicate)")
                continue
                
            # Copy to actual destination
            try:
                shutil.copy2(tmp_dst, dst_path)
            except OSError as e:
                print(f"    Failed to copy to destination: {e}")
                continue
                
            resized_hashes.add(out_hash)
            size = os.path.getsize(dst_path)
            print(f"    Saved: {dst_name} ({size // 1024} KB)")
            
            idx += 1
            resized_added += 1

    print("\n" + "=" * 60)
    print("                    RESIZE SUMMARY")
    print("=" * 60)
    print(f"Added Resized GIFs : {resized_added}")
    print(f"Total on Board     : {current_resized_count + resized_added} / {args.resize_count}")
    print(f"Output Directory   : {args.out_dir}/")
    print(f"Command to Copy    : cp -r {args.out_dir} /Volumes/<SD>/cats")
    print("=" * 60)


if __name__ == "__main__":
    main()
