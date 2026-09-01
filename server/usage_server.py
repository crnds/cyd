#!/usr/bin/env python3
"""Serves Claude Code token usage stats parsed from ~/.claude/projects/*/*.jsonl."""

import glob
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG_GLOB = os.path.expanduser("~/.claude/projects/*/*.jsonl")
PORT = 8787
MAX_BODY_BYTES = 4096  # POST bodies here are tiny JSON; anything bigger is bogus/abusive

# ── Note page (board page 6) ──────────────────────────────────────────────
# Free text authored in note.html and rendered in the right-hand pane of the
# board's Note page. It rides along inside /api/usage rather than getting its
# own endpoint, so the board needs no second HTTP call and gets SD caching for
# free via the existing /last_usage.json replay.
NOTE_PATH = os.path.expanduser("~/.cyd_note.json")
# The board's pane draws at most 24 cols x 19 rows = 456 glyphs at text size 1.
# 480 sits just above that ceiling (headroom for newlines) and comfortably
# under the firmware's 512-byte buffer. Because sanitize_note() forces ASCII,
# characters == bytes and the two caps are directly comparable.
NOTE_MAX_CHARS = 480
NOTE_HTML_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "note.html"
)
# note.html is served from this same origin, so its POST carries an Origin
# header — the blanket "reject any Origin" rule /api/battery-save uses (see
# do_POST) can't be reused here. Allowlist the origins this server can
# legitimately be reached on instead, plus "null" for the file:// dev case.
NOTE_ALLOWED_ORIGINS = {
    f"http://127.0.0.1:{PORT}",
    f"http://localhost:{PORT}",
    "null",
}
# 30s on AC: token pages update from board polls of cached aggregates, so the
# ingest lag is invisible on screen, and this halves the scan wakeups + disk
# glob/stat over ~200 jsonl files vs. the old 15s — the biggest wakeup source.
SCAN_INTERVAL_SEC = 30
# On battery (still serving): slow the jsonl tail further.
SCAN_INTERVAL_BATTERY_SEC = 30
RETENTION = timedelta(days=8)
ACTIVE_WINDOW_SEC = 180
# Assumed model context size for the "context window" percent — the jsonl
# events don't carry the model's actual limit, so 200K is hardcoded.
CONTEXT_WINDOW_TOKENS = 200000

# Raw per-message events are aggregated into fixed-width time buckets rather
# than kept individually — a bucket's key space is bounded by RETENTION (~2300
# buckets at 8 days), whereas the old per-event list grew without bound as
# usage volume grew, and build_report() re-scanned the whole thing on every
# board poll. 5 minutes is far finer than anything displayed (day/hour
# granularity), so bucket-boundary rounding is invisible on screen.
BUCKET_SEC = 300

# Approximate $/M tokens by model family. Not exact billing — cache_creation
# and cache_read are rough multipliers of the input rate.
PRICING = {
    "opus": {"input": 15.0, "output": 75.0},
    "sonnet": {"input": 3.0, "output": 15.0},
    "haiku": {"input": 0.8, "output": 4.0},
    "fable": {"input": 10.0, "output": 50.0},
}
DEFAULT_PRICING = PRICING["sonnet"]

STATE = {
    "offsets": {},      # file path -> byte offset already consumed
    "buckets": {},      # bucket_epoch -> {(project, model): {tokens, cost}}
    "seen": {},         # (message_id, request_id) -> ts, dedupes streamed rewrites
    "last_ts": None,    # exact ts of the newest ingested event (active_now)
    "aggregates": None, # cached output of compute_aggregates(), refreshed once per scan
    "clients": {},      # client ip -> last request ts, shows who is polling us
    "limits": None,     # latest plan-limit snapshot from the OAuth usage endpoint
    "context": None,    # {tokens, ts} — newest assistant event's context size
    "btc": None,        # {price} from Binance — fetched here so the board needs no TLS
    # Bangkok weather for the status card + Weather page: current + next-6h
    # hourly + next-5d daily, fetched here so the board needs no TLS.
    "weather": None,
    "aqi": None,        # {aqi} Bangkok AQI from aqicn.org — fetched here so the board needs no TLS
    # Power / battery-save. battery_guard_loop writes on_ac/percent/paused;
    # battery_save_manual is set by POST /api/battery-save (control panel):
    #   None  → auto (follow AC: unplugged = save on)
    #   True  → force low-power cadences on
    #   False → force full-speed cadences (even on battery)
    # on_ac=True until the first pmset sample so desktop Macs stay full-speed.
    "power": {
        "on_ac": True,
        "percent": None,
        "paused": False,
        "battery_save_manual": None,
    },
    # Note page text + board text size (1-3). Seeded from NOTE_PATH at startup
    # by load_note() and rewritten there on every POST /api/note, so a server
    # restart doesn't wipe what's on the board. Never None — an empty string
    # means "cleared", which the board must be able to tell from "absent".
    "note": {"text": "", "size": 1},
    # Only wakes a loop that is idle-parked in activity_gated_loop's own
    # clear-then-wait (see its comment below) -- it is set on every client
    # poll (do_GET) and every power-state change, both far more often than
    # each loop's actual fetch cadence, so it must NOT also interrupt a
    # loop's in-flight interval sleep (each body() computes and sleeps its
    # own sleep_sec — e.g. limits_loop's 120s/300s — via plain time.sleep,
    # deliberately not activity_event.wait). Doing so would fire fetches on
    # every poll instead of on-interval, risking 429s on the OAuth endpoint.
    "activity_event": threading.Event(),
    "lock": threading.Lock(),
}

CLIENT_RETENTION = timedelta(hours=24)
# How long after the last board/sim poll we treat the Mac as "no clients".
CLIENT_ACTIVE_SEC = 180
# Idle park timeout while clients are absent but the HTTP server is still up.
IDLE_WAIT_SEC = 60
# Harder park while battery_guard has unbound the socket — nothing can wake us
# via HTTP, so don't spin every minute just to re-check is_client_active.
PAUSED_WAIT_SEC = 300


def is_client_active():
    now = datetime.now(timezone.utc)
    with STATE["lock"]:
        for ip, ts in STATE["clients"].items():
            if (now - ts).total_seconds() <= CLIENT_ACTIVE_SEC:
                return True
    return False


def is_http_paused():
    with STATE["lock"]:
        return bool(STATE["power"].get("paused"))


def power_snapshot():
    # Read-only view of power + effective battery_save for API/status.
    with STATE["lock"]:
        p = dict(STATE["power"])
    on_ac = bool(p.get("on_ac", True))
    manual = p.get("battery_save_manual")  # None | True | False
    if manual is not None:
        battery_save = bool(manual)
        source = "manual"
    else:
        battery_save = not on_ac
        source = "auto"
    return {
        "on_ac": on_ac,
        "percent": p.get("percent"),
        "paused": bool(p.get("paused")),
        "battery_save": battery_save,
        "battery_save_manual": manual,
        "source": source,
    }


def battery_save_active():
    # Low-power cadences (scan/limits/market). True when the Mac is on
    # battery (auto) or when the control panel forced battery-save on.
    return power_snapshot()["battery_save"]


def set_battery_save_manual(enabled):
    # enabled True/False forces battery-save on/off; None clears the
    # override back to automatic (derived from on_ac). Wake loops so
    # intervals change without waiting out a sleep.
    with STATE["lock"]:
        p = dict(STATE["power"])
        p["battery_save_manual"] = enabled
        STATE["power"] = p
    STATE["activity_event"].set()
    log_err("battery_save: manual override %s"
            % ("on" if enabled is True else "off" if enabled is False else "cleared (auto)"))


def activity_gated_loop(body):
    # Shared "skip work while no client has polled recently" wrapper for
    # scan_loop/limits_loop/market_loop. The first iteration always runs (so
    # state is populated even before any client connects); after that, idle
    # stretches park on activity_event instead of spinning a poll loop.
    # While battery_guard has unbound the listener, park harder — no HTTP
    # client can set the event until the socket is rebound.
    first_run = True
    while True:
        if not first_run and (is_http_paused() or not is_client_active()):
            # clear-then-recheck: a poll between is_client_active and clear
            # would otherwise lose its wake signal for a full idle timeout.
            STATE["activity_event"].clear()
            if is_http_paused():
                STATE["activity_event"].wait(timeout=PAUSED_WAIT_SEC)
                continue
            if is_client_active():
                continue
            STATE["activity_event"].wait(timeout=IDLE_WAIT_SEC)
            continue
        first_run = False
        body()


# 180s on AC: the display's countdown stays smooth regardless — build_report()
# recomputes resets_in_sec per request from the last snapshot's epoch — so only
# the percent lags slightly, and this cuts the OAuth HTTPS + `security` keychain
# subprocess forks by a third.
LIMITS_INTERVAL_SEC = 180
# On battery (still serving — above BATTERY_LOW_PCT): stretch OAuth a bit.
# Display already recomputes resets_in_sec per request from the last snapshot.
LIMITS_INTERVAL_BATTERY_SEC = 300
# The OAuth endpoint rate-limits aggressive polling; after a 429, back off
# hard — the last-good snapshot plus per-request resets_in_sec keeps the
# board's display fresh in the meantime. Retry-After is respected but clamped:
# the endpoint has sent multi-hour values, which parked this loop (and froze
# the displayed percent) until the next server restart.
LIMITS_429_BACKOFF_SEC = 600
LIMITS_429_BACKOFF_MAX_SEC = 1800
OAUTH_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

# BTC + Bangkok weather are fetched by the Mac (which has ample RAM + a TLS
# stack) and handed to the board inside /api/usage. The CYD has no PSRAM, so its
# 154KB framebuffer leaves too little contiguous heap for an on-device mbedTLS
# handshake — proxying these through the plain-HTTP endpoint it already polls
# avoids TLS on the board entirely.
BTC_URL = "https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT"
# Open-Meteo: current + hourly + daily so the board can render the Weather
# page (next 6h + next 5 days) without a second HTTPS call.
WEATHER_URL = (
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=13.7563&longitude=100.5018"
    "&current=temperature_2m,weather_code"
    "&hourly=temperature_2m,weather_code"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&timezone=Asia%2FBangkok&forecast_days=6"
)
# aqicn.org (World Air Quality Index project) — same Bangkok coords as the
# weather fetch above, geo-based so it's not tied to one named station.
AQICN_URL = "https://api.waqi.info/feed/geo:13.7563;100.5018/?token={token}"
AQI_INTERVAL_SEC = 900
AQI_INTERVAL_BATTERY_SEC = 1800
# 120s is plenty for a 320px ticker tile — this loop's TLS handshakes are the
# main new radio load since BTC/weather moved off the board, and 2-min freshness
# reads identically on screen. (The old 10s cadence was ~8640 HTTPS/day of
# radio/TLS work that never changed what the board could usefully show.)
# On battery: stretch further (3 min BTC / 30 min weather) so the Mac isn't
# doing continuous HTTPS while trying to idle; pairs with the board's
# Battery Save 2 min usage-poll floor.
BTC_INTERVAL_SEC = 120
BTC_INTERVAL_BATTERY_SEC = 180
WEATHER_INTERVAL_SEC = 600
WEATHER_INTERVAL_BATTERY_SEC = 1800

# The CYD polls every ~20s nonstop; that's frequent enough that macOS never
# accumulates a long idle gap and just never commits to real system sleep.
# Harmless on AC power, but if this Mac is ever left running unattended on
# battery it silently drains to empty overnight instead of sleeping. Pausing
# (fully closing) the listening socket below this threshold lets idle sleep
# proceed; the board already has a graceful OFFLINE/cat-mode fallback for
# exactly this "server unreachable" case.
BATTERY_LOW_PCT = 50
# 120s: BATTERY_LOW_PCT (50%) is a huge margin, so a 2-min reaction to being
# unplugged is still safe, and this halves the `pmset` subprocess forks.
BATTERY_CHECK_INTERVAL_SEC = 120


def battery_status():
    # Returns (percent, on_ac) or None (desktop Macs / parse failure / no
    # battery report yet). Parsed from `pmset -g batt` text since the stdlib
    # has no battery API; on_ac comes from the first line, percent from the
    # "NN%" in the battery detail line below it.
    try:
        out = subprocess.run(["/usr/bin/pmset", "-g", "batt"],
                              capture_output=True, text=True, timeout=5)
        if out.returncode != 0:
            return None
        lines = out.stdout.splitlines()
        on_ac = "AC Power" in (lines[0] if lines else "")
        for line in lines[1:]:
            idx = line.find("%")
            if idx == -1:
                continue
            start = idx
            while start > 0 and line[start - 1].isdigit():
                start -= 1
            if start == idx:
                continue
            return int(line[start:idx]), on_ac
        return None
    except Exception:
        return None


def local_tz_name():
    try:
        link = os.readlink("/etc/localtime")
        return link.split("zoneinfo/")[-1]
    except OSError:
        return ""


TZ_NAME = local_tz_name()


def fmt_reset_time(iso_ts, with_date):
    # Time only, no timezone suffix — the CYD screen is 320px wide and the
    # reset line is rendered at a large, readable size. TZ_NAME is exposed
    # separately in the JSON for anyone who wants it.
    dt = datetime.fromisoformat(iso_ts.replace("Z", "+00:00")).astimezone()
    t = dt.strftime("%H:%M")
    return f"{dt.strftime('%b')} {dt.day}, {t}" if with_date else t


def log_err(msg):
    # stderr goes to /tmp/cydusage.err via launchd; without timestamps the
    # log can't answer "when did this loop last run", which is the first
    # question when the displayed percent goes stale.
    print(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {msg}",
          file=sys.stderr, flush=True)


def oauth_token():
    # Read the Claude Code OAuth token from the macOS Keychain at request
    # time; it is never written to disk or included in responses.
    out = subprocess.run(
        ["/usr/bin/security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
        capture_output=True, text=True, timeout=10,
    )
    if out.returncode != 0:
        return None
    creds = json.loads(out.stdout).get("claudeAiOauth", {})
    # An expired access token would just 401 (only the CLI can refresh it) —
    # skip the request rather than burn the endpoint's rate limit on it.
    expires_at_ms = creds.get("expiresAt")
    if expires_at_ms and expires_at_ms / 1000 <= time.time():
        return None
    return creds.get("accessToken")


def fetch_limits():
    token = oauth_token()
    if not token:
        return None
    req = urllib.request.Request(OAUTH_USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
    })
    with urllib.request.urlopen(req, timeout=10) as resp:
        doc = json.loads(resp.read())
    if "five_hour" not in doc and "seven_day" not in doc:
        return None
    session = doc.get("five_hour") or {}
    week = doc.get("seven_day") or {}
    now = datetime.now().astimezone()

    def window(win, with_date):
        # resets_at is null between windows (right after a reset, before the
        # next activity opens a new one) — that means 0% used, no countdown.
        # Rejecting it wholesale kept the stale pre-reset snapshot on screen.
        if not win.get("resets_at"):
            return {"percent": round(win.get("utilization") or 0), "resets": ""}
        reset_dt = datetime.fromisoformat(win["resets_at"].replace("Z", "+00:00"))
        return {
            "percent": round(win.get("utilization") or 0),
            "resets": fmt_reset_time(
                win["resets_at"],
                with_date=with_date and reset_dt.astimezone().date() != now.date()),
            # Raw epoch so resets_in_sec can be computed fresh per request
            # even when this snapshot is minutes old.
            "resets_at_epoch": reset_dt.timestamp(),
        }

    # Per-model weekly limit (e.g. "Weekly - Fable") from the limits[] array.
    # May disappear from the response entirely — clients hide the row on null.
    week_model = None
    for lim in (doc.get("limits") or []):
        if lim.get("kind") == "weekly_scoped" and lim.get("percent") is not None:
            scope = lim.get("scope") or {}
            name = ((scope.get("model") or {}).get("display_name")) or ""
            week_model = window(
                {"utilization": lim["percent"], "resets_at": lim.get("resets_at")},
                with_date=True)
            week_model["name"] = name
            break

    # Extra-usage credits ("$0.41 of $10.00"). Prefer the structured `spend`
    # object; fall back to the flatter `extra_usage`. Null when disabled.
    credits = None
    spend = doc.get("spend") or {}
    extra = doc.get("extra_usage") or {}
    if spend.get("enabled") and spend.get("used"):
        exp = spend["used"].get("exponent", 2)
        used = spend["used"].get("amount_minor", 0) / (10 ** exp)
        limit = (spend.get("limit") or {}).get("amount_minor", 0) / (10 ** exp)
        pct = spend.get("percent")
        if pct is None:
            pct = round(used / limit * 100) if limit else 0
        credits = {"used": round(used, 2), "limit": round(limit, 2),
                   "percent": round(pct)}
    elif extra.get("is_enabled") and extra.get("monthly_limit") is not None:
        dp = extra.get("decimal_places", 2)
        used = (extra.get("used_credits") or 0) / (10 ** dp)
        limit = (extra.get("monthly_limit") or 0) / (10 ** dp)
        pct = extra.get("utilization")
        if pct is None:
            pct = used / limit * 100 if limit else 0
        credits = {"used": round(used, 2), "limit": round(limit, 2),
                   "percent": round(pct)}

    return {
        "tz": TZ_NAME,
        # Raw epoch of this fetch; the handler turns it into age_sec per
        # request so clients can see when the snapshot has gone stale.
        "fetched_at_epoch": time.time(),
        "session": window(session, with_date=False),
        "week": window(week, with_date=True),
        "week_model": week_model,
        "credits": credits,
    }


def limits_interval():
    return LIMITS_INTERVAL_BATTERY_SEC if battery_save_active() else LIMITS_INTERVAL_SEC


def limits_loop():
    was_empty = False
    last_fetch = 0.0

    def body():
        nonlocal was_empty, last_fetch
        now = time.time()
        interval = limits_interval()
        sleep_sec = 5
        if now - last_fetch >= interval:
            try:
                limits = fetch_limits()
                if limits:
                    STATE["limits"] = limits
                    last_fetch = now
                    if was_empty:
                        log_err("limits_loop: recovered, snapshot refreshed")
                    was_empty = False
                else:
                    # No usable token or null fields in the response; keep the
                    # last-good snapshot. Log the transition only — this repeats
                    # every cycle until the CLI refreshes the keychain token.
                    if not was_empty:
                        log_err("limits_loop: no snapshot (missing/expired token "
                                "or null fields); keeping last-good")
                    was_empty = True
            except Exception as e:
                # keep the last good snapshot; report shows it as-is
                log_err(f"limits_loop: {type(e).__name__}: {e}")
                if isinstance(e, urllib.error.HTTPError) and e.code == 429:
                    try:
                        retry_after = int(e.headers.get("Retry-After") or 0)
                    except ValueError:
                        retry_after = 0
                    sleep_sec = min(max(LIMITS_429_BACKOFF_SEC, retry_after),
                                    LIMITS_429_BACKOFF_MAX_SEC)
                    log_err(f"limits_loop: backing off {sleep_sec}s "
                            f"(Retry-After: {e.headers.get('Retry-After')!r})")
                    last_fetch = now - interval + sleep_sec
            else:
                sleep_sec = interval
        else:
            sleep_sec = max(1, int(interval - (now - last_fetch)))

        time.sleep(sleep_sec)

    activity_gated_loop(body)


_SECRETS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "secrets.local.json")
_aqicn_token_warned = False


def aqicn_token():
    # AQICN_TOKEN env var, then secrets.local.json (repo root, gitignored) --
    # same lookup order pull_giphy_cats.py uses for its GIPHY_API_KEY, so a
    # missing token here is either unset (never fetched) or added there.
    global _aqicn_token_warned
    env_token = os.environ.get("AQICN_TOKEN")
    if env_token:
        return env_token
    try:
        with open(_SECRETS_PATH) as f:
            token = (json.load(f) or {}).get("AQICN_TOKEN")
    except (OSError, ValueError):
        token = None
    if not token and not _aqicn_token_warned:
        _aqicn_token_warned = True
        log_err("aqi: no AQICN_TOKEN in env or secrets.local.json -- "
                "skipping AQI fetch (get a free token at https://aqicn.org/data-platform/token/)")
    return token


def fetch_aqi():
    token = aqicn_token()
    if not token:
        return None
    req = urllib.request.Request(AQICN_URL.format(token=token), headers={"User-Agent": "cydusage"})
    with urllib.request.urlopen(req, timeout=8) as resp:
        doc = json.loads(resp.read())
    if doc.get("status") != "ok":
        return None
    aqi = (doc.get("data") or {}).get("aqi")
    if not isinstance(aqi, (int, float)) or aqi < 0:
        return None
    return {"aqi": round(aqi)}


def fetch_btc():
    req = urllib.request.Request(BTC_URL, headers={"User-Agent": "cydusage"})
    with urllib.request.urlopen(req, timeout=8) as resp:
        doc = json.loads(resp.read())
    price = float(doc.get("lastPrice") or 0)
    # Price only — changePct was never drawn on board/sim; omit to keep the
    # /api/usage payload lean (24hr endpoint still used for lastPrice).
    return {"price": price} if price > 0 else None


def wmo_condition(code):
    # Short condition labels for the Weather page header. Buckets match
    # drawWeatherIcon() so icon + text always describe the same condition.
    if code is None or code < 0:
        return ""
    if code == 0:
        return "Clear"
    if code == 1:
        return "Mainly Clear"
    if code == 2:
        return "Partly Cloudy"
    if code == 3:
        return "Overcast"
    if code in (45, 48):
        return "Fog"
    if 51 <= code <= 57:
        return "Drizzle"
    if 61 <= code <= 67:
        return "Rain"
    if 71 <= code <= 77:
        return "Snow"
    if 80 <= code <= 82:
        return "Showers"
    if 85 <= code <= 86:
        return "Snow Showers"
    if code >= 95:
        return "Thunderstorm"
    return "Cloudy"


def _iso_hour(ts):
    # "2026-07-20T17:00" or "2026-07-20 17:00" -> hour int, or None.
    if not ts or len(ts) < 13:
        return None
    try:
        return int(ts[11:13])
    except (TypeError, ValueError):
        return None


def _iso_wday(date_str):
    # YYYY-MM-DD -> tm_wday style (0=Sun .. 6=Sat).
    try:
        dt = datetime.strptime(date_str[:10], "%Y-%m-%d")
        return (dt.weekday() + 1) % 7
    except (TypeError, ValueError):
        return 0


def weather_payload(temp_c, code, high=None, low=None, hourly=None, daily=None):
    # Shared shape for both providers — the board/simulator contract.
    # place is fixed Bangkok (hardcoded locale); not drawn on device.
    out = {
        "tempC": temp_c,
        "code": code if code is not None else -1,
        "condition": wmo_condition(code if code is not None else -1),
        "high": high if high is not None else None,
        "low": low if low is not None else None,
        "hourly": hourly or [],
        "daily": daily or [],
    }
    return out


def fetch_weather_openmeteo():
    with urllib.request.urlopen(WEATHER_URL, timeout=10) as resp:
        doc = json.loads(resp.read())
    cur = doc.get("current") or {}
    if cur.get("temperature_2m") is None:
        return None
    code = cur.get("weather_code", -1)
    temp_c = cur["temperature_2m"]

    # Next 6 hours starting at the current hour (or the next future slot if
    # the exact current hour is missing from the series).
    hourly_out = []
    hdoc = doc.get("hourly") or {}
    h_times = hdoc.get("time") or []
    h_temps = hdoc.get("temperature_2m") or []
    h_codes = hdoc.get("weather_code") or []
    cur_time = cur.get("time") or ""
    # If no slot >= cur_time is found (the hourly series doesn't reach "now"),
    # falling back to 0 would silently show the oldest/stale hours at the
    # start of the window instead. Default past the end so the range below
    # comes up empty -- no hourly row beats a wrong one.
    start_i = len(h_times)
    for i, t in enumerate(h_times):
        if t[:13] >= cur_time[:13]:
            start_i = i
            break
    for i in range(start_i, min(start_i + 6, len(h_times))):
        h = _iso_hour(h_times[i])
        if h is None or i >= len(h_temps) or h_temps[i] is None:
            continue
        hc = h_codes[i] if i < len(h_codes) else -1
        hourly_out.append({
            "h": h,
            "tempC": round(h_temps[i]),
            "code": hc if hc is not None else -1,
        })

    # Today + next 4 days (5 rows on the Weather page).
    daily_out = []
    ddoc = doc.get("daily") or {}
    d_times = ddoc.get("time") or []
    d_max = ddoc.get("temperature_2m_max") or []
    d_min = ddoc.get("temperature_2m_min") or []
    d_codes = ddoc.get("weather_code") or []
    high = low = None
    for i in range(min(5, len(d_times))):
        hi = d_max[i] if i < len(d_max) else None
        lo = d_min[i] if i < len(d_min) else None
        dc = d_codes[i] if i < len(d_codes) else -1
        if i == 0:
            high = None if hi is None else round(hi)
            low = None if lo is None else round(lo)
        daily_out.append({
            "wd": _iso_wday(d_times[i]),
            "high": None if hi is None else round(hi),
            "low": None if lo is None else round(lo),
            "code": dc if dc is not None else -1,
        })

    return weather_payload(temp_c, code, high=high, low=low,
                           hourly=hourly_out, daily=daily_out)


def fetch_weather():
    try:
        return fetch_weather_openmeteo()
    except Exception as e:
        log_err(f"weather: Open-Meteo failed: {type(e).__name__}: {e}")
        return None


def market_intervals():
    if battery_save_active():
        return BTC_INTERVAL_BATTERY_SEC, WEATHER_INTERVAL_BATTERY_SEC, AQI_INTERVAL_BATTERY_SEC
    return BTC_INTERVAL_SEC, WEATHER_INTERVAL_SEC, AQI_INTERVAL_SEC


def market_loop():
    # BTC ~60s on AC / ~3 min on battery; weather 10 min / 30 min; AQI 15 min /
    # 30 min (an hourly-updating station gains nothing from polling faster).
    # Each keeps its last good value on failure so a transient outage doesn't
    # blank tiles.
    last_weather = 0.0
    last_aqi = 0.0

    def body():
        nonlocal last_weather, last_aqi
        btc_iv, weather_iv, aqi_iv = market_intervals()
        try:
            btc = fetch_btc()
            if btc:
                STATE["btc"] = btc
        except Exception as e:
            log_err(f"market_loop btc: {type(e).__name__}: {e}")
        now = time.time()
        if now - last_weather >= weather_iv:
            last_weather = now
            try:
                weather = fetch_weather()
                if weather:
                    STATE["weather"] = weather
            except Exception as e:
                log_err(f"market_loop weather: {type(e).__name__}: {e}")
        if now - last_aqi >= aqi_iv:
            last_aqi = now
            try:
                aqi = fetch_aqi()
                if aqi:
                    STATE["aqi"] = aqi
            except Exception as e:
                log_err(f"market_loop aqi: {type(e).__name__}: {e}")
        time.sleep(btc_iv)

    activity_gated_loop(body)


def pricing_for_model(model):
    model = (model or "").lower()
    for key, rates in PRICING.items():
        if key in model:
            return rates
    return DEFAULT_PRICING


def estimate_cost(usage, model):
    rates = pricing_for_model(model)
    input_tok = usage.get("input_tokens", 0)
    output_tok = usage.get("output_tokens", 0)
    cache_creation = usage.get("cache_creation_input_tokens", 0)
    cache_read = usage.get("cache_read_input_tokens", 0)
    cost = (
        input_tok * rates["input"]
        + output_tok * rates["output"]
        + cache_creation * rates["input"] * 1.25
        + cache_read * rates["input"] * 0.1
    ) / 1_000_000
    return cost


def model_family(model):
    # "claude-opus-4-8" -> "opus", "claude-haiku-4-5-20251001" -> "haiku"
    if not model:
        return "other"
    m = model.lower()
    if m.startswith("claude-"):
        m = m[len("claude-"):]
    return m.split("-")[0] or "other"


def total_tokens(usage):
    return (
        usage.get("input_tokens", 0)
        + usage.get("output_tokens", 0)
        + usage.get("cache_creation_input_tokens", 0)
        + usage.get("cache_read_input_tokens", 0)
    )


def project_name(cwd):
    if not cwd:
        return "unknown"
    return os.path.basename(cwd.rstrip("/")) or cwd


def bucket_epoch_for(ts):
    return int(ts.timestamp() // BUCKET_SEC) * BUCKET_SEC


def scan_once():
    # new_buckets: bucket_epoch -> {(project, model): {tokens, cost}}. Bucketed
    # at ingest time (rather than keeping a growing list of raw events) so
    # memory and the per-request aggregation work below are bounded by
    # RETENTION, not by how much has been used.
    new_buckets = {}
    newest_ctx = None  # {tokens, ts} of the newest event in this batch
    newest_ts = None   # exact ts of the newest event in this batch (bucketing
                        # would blur active_now's 180s window otherwise)
    current_paths = set(glob.glob(LOG_GLOB))
    for path in current_paths:
        offset = STATE["offsets"].get(path)
        if offset is None:
            try:
                mtime = os.path.getmtime(path)
                size = os.path.getsize(path)
            except OSError:
                continue
            if mtime < time.time() - RETENTION.total_seconds():
                STATE["offsets"][path] = size
                continue
            offset = 0
        else:
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            if size < offset:
                offset = 0  # file was truncated/replaced; re-read from the start

        if size == offset:
            continue
        try:
            with open(path, "rb") as f:
                f.seek(offset)
                data = f.read()
        except OSError:
            continue
        # Only advance past lines that end in a newline — a line still being
        # written when we read it would otherwise be skipped forever, since
        # the offset would jump past the partial bytes before they're complete.
        if data.endswith(b"\n"):
            consumable, remainder = data, b""
        else:
            split_at = data.rfind(b"\n") + 1
            consumable, remainder = data[:split_at], data[split_at:]
        for raw_line in consumable.split(b"\n"):
            line = raw_line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            message = entry.get("message") or {}
            usage = message.get("usage")
            if not usage:
                continue
            ts_raw = entry.get("timestamp")
            if not ts_raw:
                continue
            try:
                ts = datetime.fromisoformat(ts_raw.replace("Z", "+00:00"))
            except ValueError:
                continue
            if ts.tzinfo is None:
                # A naive ts here would later raise TypeError when compared
                # against the aware `cutoff`/`now` below -- and by then this
                # file's offset has already advanced, permanently dropping
                # these events instead of retrying them next scan.
                ts = ts.replace(tzinfo=timezone.utc)
            # The same assistant message is often written multiple
            # times while streaming, with identical usage — count once.
            msg_id = message.get("id")
            if msg_id:
                key = (msg_id, entry.get("requestId"))
                if key in STATE["seen"]:
                    continue
                STATE["seen"][key] = ts

            project = project_name(entry.get("cwd"))
            model = model_family(message.get("model"))
            tokens = total_tokens(usage)
            cost = estimate_cost(usage, message.get("model"))

            dims = new_buckets.setdefault(bucket_epoch_for(ts), {})
            d = dims.setdefault((project, model), {"tokens": 0, "cost": 0.0})
            d["tokens"] += tokens
            d["cost"] += cost

            # Context window of the latest session = the newest assistant
            # event's prompt size (input + both cache tiers, no output).
            ctx_tokens = (
                (usage.get("input_tokens") or 0)
                + (usage.get("cache_read_input_tokens") or 0)
                + (usage.get("cache_creation_input_tokens") or 0))
            if newest_ctx is None or ts > newest_ctx["ts"]:
                newest_ctx = {"tokens": ctx_tokens, "ts": ts}
            if newest_ts is None or ts > newest_ts:
                newest_ts = ts
        STATE["offsets"][path] = offset + len(consumable)

    # Drop offsets for paths that no longer exist (deleted/rotated project
    # logs) -- otherwise this dict grows for the life of the process. Safe to
    # re-read from scratch if a path reappears: "seen" dedup (keyed on
    # message id + requestId, retained for RETENTION) still catches re-ingested
    # duplicates.
    stale_paths = set(STATE["offsets"]) - current_paths
    for path in stale_paths:
        del STATE["offsets"][path]

    # Returned to scan_loop so it can skip the compute_aggregates() rollup on
    # scans that ingested nothing (most scans, when no Claude session is
    # active) -- see its day-rollover note.
    if not new_buckets:
        return False
    with STATE["lock"]:
        for bucket_epoch, dims in new_buckets.items():
            target = STATE["buckets"].setdefault(bucket_epoch, {})
            for key, v in dims.items():
                d = target.setdefault(key, {"tokens": 0, "cost": 0.0})
                d["tokens"] += v["tokens"]
                d["cost"] += v["cost"]
        cutoff = datetime.now(timezone.utc) - RETENTION
        cutoff_epoch = bucket_epoch_for(cutoff)
        STATE["buckets"] = {b: v for b, v in STATE["buckets"].items() if b >= cutoff_epoch}
        STATE["seen"] = {k: ts for k, ts in STATE["seen"].items() if ts >= cutoff}
        prev = STATE["context"]
        if newest_ctx and (prev is None or newest_ctx["ts"] > prev["ts"]):
            STATE["context"] = newest_ctx
        if newest_ts and (STATE["last_ts"] is None or newest_ts > STATE["last_ts"]):
            STATE["last_ts"] = newest_ts
    return True


# Logged hourly from scan_loop: a lightweight heartbeat of the bucketed
# state's size, so a runaway (e.g. buckets not pruning correctly) shows up in
# /tmp/cydusage.err rather than silently growing the process's RSS.
STATS_LOG_INTERVAL_SEC = 3600
_last_stats_log = 0.0


def maybe_log_stats():
    global _last_stats_log
    now = time.time()
    if now - _last_stats_log < STATS_LOG_INTERVAL_SEC:
        return
    _last_stats_log = now
    with STATE["lock"]:
        n_buckets = len(STATE["buckets"])
        n_seen = len(STATE["seen"])
        n_clients = len(STATE["clients"])
    log_err(f"stats: buckets={n_buckets} seen={n_seen} clients={n_clients}")


def compute_aggregates():
    # Rolls the bucketed state up into everything build_report() needs that
    # doesn't have to be fresh on every single request (today/week totals,
    # top projects, 7-day trend). Called once per scan cycle (~15s) instead
    # of once per HTTP request. last5h / models[] were dropped — no board,
    # simulator, or control-panel consumer.
    now = datetime.now(timezone.utc)
    local_today = datetime.now().astimezone().date()

    with STATE["lock"]:
        buckets = list(STATE["buckets"].items())

    today_tokens = today_cost = 0
    week_tokens = week_cost = 0
    project_totals = {}
    day_totals = {}

    seven_d_ago = now - timedelta(days=7)

    for bucket_epoch, dims in buckets:
        bucket_dt = datetime.fromtimestamp(bucket_epoch, tz=timezone.utc)
        bucket_tokens = sum(v["tokens"] for v in dims.values())
        bucket_cost = sum(v["cost"] for v in dims.values())
        local_date = bucket_dt.astimezone().date()

        if local_date == local_today:
            today_tokens += bucket_tokens
            today_cost += bucket_cost

        if bucket_dt >= seven_d_ago:
            week_tokens += bucket_tokens
            week_cost += bucket_cost
            for (project, _model), v in dims.items():
                project_totals[project] = project_totals.get(project, 0) + v["tokens"]
            day_totals[local_date] = day_totals.get(local_date, 0) + bucket_tokens

    top_projects = sorted(project_totals.items(), key=lambda kv: kv[1], reverse=True)[:5]

    trend = []
    for i in range(6, -1, -1):
        d = local_today - timedelta(days=i)
        trend.append(day_totals.get(d, 0))

    aggregates = {
        "today": {"tokens": today_tokens, "cost": round(today_cost, 4)},
        "week": {"tokens": week_tokens, "cost": round(week_cost, 4)},
        "projects": [{"name": name, "tokens": tokens} for name, tokens in top_projects],
        "trend": trend,
    }
    with STATE["lock"]:
        STATE["aggregates"] = aggregates


def scan_interval():
    # On battery, scan half as often — jsonl tails are cheap, but glob +
    # recompute every 15s is pure waste when the Mac is trying to idle.
    return SCAN_INTERVAL_BATTERY_SEC if battery_save_active() else SCAN_INTERVAL_SEC


def scan_loop():
    # Recompute the rollup only when scan_once() ingested new data, OR when the
    # local calendar day has rolled over (so today/trend advance at midnight even
    # through a long idle stretch with no new events -- a plain "new data only"
    # gate would otherwise freeze "today" at yesterday's total until the next
    # event). This skips the every-scan compute_aggregates() during idle, which
    # was pure repeated work re-deriving identical output.
    last_agg_date = datetime.now().astimezone().date()

    def body():
        nonlocal last_agg_date
        try:
            changed = scan_once()
            today = datetime.now().astimezone().date()
            if changed or today != last_agg_date:
                compute_aggregates()
                last_agg_date = today
            maybe_log_stats()
        except Exception as e:
            # never let a transient error (e.g. during sleep/wake) kill the thread
            log_err(f"scan_loop: {type(e).__name__}: {e}")
        time.sleep(scan_interval())

    activity_gated_loop(body)


def sanitize_note(text):
    """Fold arbitrary editor text down to what the board can actually draw.

    The CYD renders with the built-in Adafruit GFX font, which only has glyphs
    for 7-bit printable ASCII. Thai, emoji and box-drawing characters have no
    glyph at all, so passing them through would paint garbage. They become "?"
    rather than being dropped, so a note never silently loses content — and
    note.html warns about the count before you ever get here.
    """
    if not isinstance(text, str):
        return ""
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\t", " ")
    out = []
    for ch in text:
        if ch == "\n" or " " <= ch <= "~":
            out.append(ch)
        else:
            out.append("?")
    return "".join(out)[:NOTE_MAX_CHARS]


def note_snapshot():
    with STATE["lock"]:
        return dict(STATE["note"])


def load_note():
    """Seed STATE["note"] from disk at startup. A missing/corrupt file is not
    an error — it just means "no note yet"."""
    try:
        with open(NOTE_PATH, "r", encoding="utf-8") as f:
            doc = json.load(f)
        note = {
            "text": sanitize_note(doc.get("text", "")),
            "size": min(3, max(1, int(doc.get("size", 1)))),
        }
    except (OSError, ValueError, TypeError):
        return
    with STATE["lock"]:
        STATE["note"] = note


def save_note(note):
    """Persist atomically so a crash mid-write can't leave a truncated file
    that load_note() would then discard on the next boot."""
    tmp = NOTE_PATH + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(note, f)
        os.replace(tmp, NOTE_PATH)
    except OSError as e:
        log_err(f"save_note: {type(e).__name__}: {e}")


def build_report():
    now = datetime.now(timezone.utc)

    with STATE["lock"]:
        agg = STATE["aggregates"] or {
            "today": {"tokens": 0, "cost": 0.0},
            "week": {"tokens": 0, "cost": 0.0},
            "projects": [], "trend": [0] * 7,
        }
        ctx = STATE["context"]
        last_ts = STATE["last_ts"]
        limits = STATE["limits"]
        # Prune clients that haven't polled in 24h so this dict (and the
        # `clients` array in every response) doesn't grow forever.
        cutoff_client = now - CLIENT_RETENTION
        STATE["clients"] = {ip: ts for ip, ts in STATE["clients"].items() if ts >= cutoff_client}
        client_snapshot = list(STATE["clients"].items())

    # active_now is archive-only on the board (not drawn); kept for SD CSV.
    active_now = last_ts is not None and (now - last_ts).total_seconds() <= ACTIVE_WINDOW_SEC

    clients = [
        {"ip": ip, "last_seen_sec": int((now - ts).total_seconds())}
        for ip, ts in sorted(client_snapshot, key=lambda kv: kv[1], reverse=True)
    ]

    # Countdown to the session reset, computed at request time so it stays
    # accurate between OAuth refreshes. Mutate per-request copies only —
    # STATE["limits"] stays pristine with internal epochs.
    if limits:
        limits = dict(limits)
        limits.pop("fetched_at_epoch", None)
        limits.pop("tz", None)  # never read by board/sim/control panel
        for key in ("session", "week", "week_model"):
            win = limits.get(key) or {}
            if not isinstance(win, dict):
                continue
            win = dict(win)
            epoch = win.pop("resets_at_epoch", None)
            if epoch:
                remaining = int(epoch - time.time())
                if remaining <= 0:
                    # Window ended; OAuth loop may not have a fresh snapshot
                    # yet. Serve reset rather than a stale percent.
                    win["percent"] = 0
                    win["resets"] = ""
                    remaining = 0
                if key in ("session", "week"):
                    win["resets_in_sec"] = remaining
            limits[key] = win

    return {
        "generated_at": now.isoformat(),
        "epoch": int(now.timestamp()),
        "today": agg["today"],
        "week": agg["week"],
        "active_now": active_now,
        "projects": agg["projects"],
        "trend": agg["trend"],
        "limits": limits,
        "context": ({"tokens": ctx["tokens"],
                     "percent": round(ctx["tokens"] / CONTEXT_WINDOW_TOKENS * 100)}
                    if ctx else None),
        "btc": STATE["btc"],
        "weather": STATE["weather"],
        "aqi": STATE["aqi"],
        # Board page 6. Always an object (never null) so an empty "text" reads
        # as "the user cleared the note" rather than "no data yet" — the
        # firmware keeps its previous value for absent keys.
        "note": STATE["note"],
        "clients": clients,
        "power": power_snapshot(),
    }


HEARTBEAT_LOG_INTERVAL_SEC = 300
_last_heartbeat_log = {}  # ip -> monotonic time of last heartbeat line logged

# Gap reporting. The throttled heartbeat above keeps the log small, but it also
# made real outages invisible: at any poll interval the log looks like one
# tidy line every 300s whether the board polled 60 times in between or zero.
# That is why a "board goes OFFLINE for 1-2 minutes" report could not be
# confirmed or dated from this log at all.
#
# So: also emit one line whenever a client returns after a suspiciously long
# silence. The board's poll interval is not known here, so it is inferred as
# the smallest gap ever seen from that IP (a client cannot poll faster than its
# own interval). A gap of GAP_MULTIPLE times that, and at least
# GAP_MIN_SEC (the firmware's own offline grace, OFFLINE_AFTER_MS), means the
# board almost certainly showed the OFFLINE screen. Low volume by construction:
# silent while things are healthy, one line per actual episode.
GAP_MIN_SEC = 60
GAP_MULTIPLE = 3
_last_seen_mono = {}  # ip -> monotonic time of that client's previous request
_min_gap_seen = {}    # ip -> smallest inter-request gap, i.e. inferred poll interval


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, body, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        # ACAO only for the file:// dev case (Origin: null) -- simulator.html
        # fetches this endpoint directly from disk. A blanket "*" would let
        # any website's JS read board usage data cross-origin over the LAN/
        # loopback; nothing else needs CORS here (the board is not a browser,
        # and server.html goes through control_server's proxy instead).
        if self.headers.get("Origin") == "null":
            self.send_header("Access-Control-Allow-Origin", "null")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, doc, status=200):
        self._send(json.dumps(doc).encode("utf-8"), "application/json", status)

    def _is_local(self):
        return self.client_address[0] in ("127.0.0.1", "::1")

    def _read_json_body(self):
        """Shared POST body reader. Returns (doc, None) or (None, True) after
        having already sent the error response."""
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length > MAX_BODY_BYTES:
            self._send_json({"ok": False, "detail": "body too large"}, status=413)
            return None, True
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            return json.loads(raw.decode("utf-8") or "{}"), None
        except (ValueError, UnicodeDecodeError):
            self._send_json({"ok": False, "detail": "invalid JSON"}, status=400)
            return None, True

    def do_OPTIONS(self):
        # Only the file:// (Origin: null) case ever preflights: note.html is
        # served same-origin, and same-origin requests skip CORS entirely.
        if self.headers.get("Origin") == "null" and self.path == "/api/note":
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "null")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()
            return
        self.send_response(404)
        self.end_headers()

    def _serve_note_html(self):
        # Read fresh from disk per request (same as control_server.py) — this
        # is a single local page, not a hot path, and it means editing
        # note.html doesn't need a server restart.
        try:
            with open(NOTE_HTML_PATH, "rb") as f:
                self._send(f.read(), "text/html; charset=utf-8")
        except OSError:
            self._send(b"note.html not found", "text/plain", status=404)

    def do_GET(self):
        # The note routes are localhost-only, matching /api/battery-save:
        # the note is writable only from this Mac, so serving the editor to
        # the LAN would just be a form that 403s on save.
        if self.path in ("/", "/note", "/note.html"):
            if not self._is_local():
                self._send(b"localhost only", "text/plain", status=403)
                return
            self._serve_note_html()
            return
        if self.path == "/api/note":
            if not self._is_local():
                self._send_json({"ok": False, "detail": "localhost only"}, status=403)
                return
            self._send_json(note_snapshot())
            return
        if self.path != "/api/usage":
            self.send_response(404)
            self.end_headers()
            return
        # Only /api/usage counts as a client poll: the client registry drives
        # active_now and un-parks the activity-gated background loops, and a
        # browser loading the note editor is not a dashboard client.
        ip = self.client_address[0]
        with STATE["lock"]:
            STATE["clients"][ip] = datetime.now(timezone.utc)
            STATE["activity_event"].set()
        if ip not in ("127.0.0.1", "::1"):
            # Heartbeat line, throttled to once per HEARTBEAT_LOG_INTERVAL_SEC
            # per client -- at the board's ~20s poll cadence an unthrottled
            # line here grows /tmp/cydusage.log by ~200KB/day for no benefit,
            # since `tail -f` just needs proof-of-life, not every single poll.
            now_mono = time.monotonic()
            stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

            prev_seen = _last_seen_mono.get(ip)
            _last_seen_mono[ip] = now_mono
            gap = None if prev_seen is None else now_mono - prev_seen
            if gap is not None:
                floor = _min_gap_seen.get(ip)
                # Update the inferred poll interval before testing, so the very
                # first gaps (which set the baseline) can't read as an outage.
                if floor is None or gap < floor:
                    _min_gap_seen[ip] = gap
                    floor = gap
                if gap >= GAP_MIN_SEC and gap >= floor * GAP_MULTIPLE:
                    # Stamping the heartbeat here also suppresses the routine
                    # line below, so an episode reports exactly once.
                    _last_heartbeat_log[ip] = now_mono
                    print(
                        f"{stamp} {ip} GET /api/usage  RESUMED after {gap:.0f}s gap "
                        f"(polls ~every {floor:.0f}s) -- board was likely OFFLINE",
                        flush=True,
                    )

            last = _last_heartbeat_log.get(ip, 0.0)
            if now_mono - last >= HEARTBEAT_LOG_INTERVAL_SEC:
                _last_heartbeat_log[ip] = now_mono
                print(f"{stamp} {ip} GET /api/usage", flush=True)
        self._send_json(build_report())

    def _do_post_note(self):
        # Note text from note.html. Localhost-only, same policy as
        # /api/battery-save: anyone on the LAN can *read* the note (it rides
        # in /api/usage, which is how the board gets it), but only this Mac
        # can change what's on the screen.
        if not self._is_local():
            self._send_json({"ok": False, "detail": "localhost only"}, status=403)
            return
        # Unlike /api/battery-save, an Origin header is expected here —
        # note.html is served from this origin, and browsers send Origin on
        # same-origin POSTs. Allowlist instead of blanket-reject, which still
        # blocks the CSRF path (a random site's origin isn't in the set).
        origin = self.headers.get("Origin")
        if origin is not None and origin not in NOTE_ALLOWED_ORIGINS:
            self._send_json({"ok": False, "detail": "forbidden origin"}, status=403)
            return
        doc, err = self._read_json_body()
        if err:
            return
        if not isinstance(doc, dict) or not isinstance(doc.get("text"), str):
            self._send_json(
                {"ok": False, "detail": "body must be {\"text\": str, \"size\": 1-3}"},
                status=400,
            )
            return
        size = doc.get("size", 1)
        if not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= 3:
            self._send_json(
                {"ok": False, "detail": "size must be an int 1-3"}, status=400
            )
            return
        note = {"text": sanitize_note(doc["text"]), "size": size}
        with STATE["lock"]:
            STATE["note"] = note
        save_note(note)
        self._send_json({"ok": True, "note": note})

    def do_POST(self):
        if self.path == "/api/note":
            self._do_post_note()
            return
        # Manual battery-save override from the localhost control panel.
        # LAN clients (the board) get 403 — power policy stays Mac-local.
        if self.path != "/api/battery-save":
            self.send_response(404)
            self.end_headers()
            return
        if not self._is_local():
            self._send_json({"ok": False, "detail": "localhost only"}, status=403)
            return
        # The only legitimate caller is control_server.py's server-side
        # urllib proxy, which never sends an Origin header. A same-machine
        # browser tab (any website, not just server.html) CAN still reach
        # 127.0.0.1 directly regardless of its own origin -- the IP check
        # above doesn't stop that CSRF path -- so any request carrying an
        # Origin header at all is rejected here.
        if self.headers.get("Origin") is not None:
            self._send_json({"ok": False, "detail": "forbidden origin"}, status=403)
            return
        doc, err = self._read_json_body()
        if err:
            return
        if not isinstance(doc, dict) or "enabled" not in doc or (
            doc.get("enabled") is not None and not isinstance(doc.get("enabled"), bool)
        ):
            self._send_json(
                {"ok": False, "detail": "body must be {\"enabled\": true|false|null}"},
                status=400,
            )
            return
        set_battery_save_manual(doc["enabled"])
        self._send_json({"ok": True, "power": power_snapshot()})


class Server(ThreadingHTTPServer):
    allow_reuse_address = True  # rebind immediately if launchd restarts us
    daemon_threads = True


def start_http_server():
    # A bare bind failure here (e.g. EADDRINUSE from the previous instance's
    # socket still tearing down across a fast launchd restart) used to raise
    # straight out of main(), crash-looping launchd every ~10s. Retry with
    # backoff instead so a transient bind failure self-heals.
    delay = 1
    while True:
        try:
            server = Server(("0.0.0.0", PORT), Handler)
            break
        except OSError as e:
            log_err(f"start_http_server: bind failed ({e}), retrying in {delay}s")
            time.sleep(delay)
            delay = min(delay * 2, 30)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Serving usage stats on http://0.0.0.0:{PORT}/api/usage")
    print(f"Note editor (localhost only) on http://127.0.0.1:{PORT}/")
    return server


def set_power_state(on_ac, percent, paused):
    # Preserve battery_save_manual across pmset samples — the control panel
    # owns that field, not the battery guard.
    with STATE["lock"]:
        prev = STATE["power"]
        STATE["power"] = {
            "on_ac": on_ac,
            "percent": percent,
            "paused": paused,
            "battery_save_manual": prev.get("battery_save_manual"),
        }
        changed = (prev.get("on_ac") != on_ac or prev.get("paused") != paused)
    if changed:
        # Wake gated loops so they pick up the new cadence / hard-park promptly
        # instead of waiting out a full idle timeout.
        STATE["activity_event"].set()


def battery_guard_loop():
    # Owns the HTTP server's lifecycle: fully unbinds the listening socket
    # while on battery below BATTERY_LOW_PCT (so the board sees OFFLINE
    # instead of the Mac's battery hitting 0%), and rebinds once the Mac is
    # back on AC or the battery has recovered. Also publishes on_ac/paused
    # into STATE["power"] so the other loops can throttle before we hard-stop.
    server = start_http_server()
    paused = False
    while True:
        time.sleep(BATTERY_CHECK_INTERVAL_SEC)
        status = battery_status()
        if status is None:
            # Desktop / unknown: keep serving, treat as AC for cadence.
            set_power_state(True, None, paused)
            continue
        percent, on_ac = status
        should_pause = (not on_ac) and percent <= BATTERY_LOW_PCT
        try:
            if should_pause and not paused:
                log_err(f"battery_guard: {percent}% on battery, pausing HTTP "
                        f"server so macOS can sleep")
                server.shutdown()
                server.server_close()
                server = None
                paused = True
            elif not should_pause and paused:
                log_err(f"battery_guard: recovered ({percent}%, "
                        f"on_ac={on_ac}), resuming HTTP server")
                server = start_http_server()
                paused = False
            set_power_state(on_ac, percent, paused)
        except Exception as e:
            log_err(f"battery_guard: {type(e).__name__}: {e}")


def main():
    load_note()
    scan_once()
    compute_aggregates()
    threading.Thread(target=scan_loop, daemon=True).start()
    threading.Thread(target=limits_loop, daemon=True).start()
    threading.Thread(target=market_loop, daemon=True).start()
    battery_guard_loop()


if __name__ == "__main__":
    main()
