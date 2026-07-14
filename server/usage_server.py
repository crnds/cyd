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
SCAN_INTERVAL_SEC = 15
RETENTION = timedelta(days=8)
ACTIVE_WINDOW_SEC = 180
# Assumed model context size for the "context window" percent — the jsonl
# events don't carry the model's actual limit, so 200K is hardcoded.
CONTEXT_WINDOW_TOKENS = 200000

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
    "events": [],       # list of {ts: datetime, project, tokens, cost}
    "seen": {},         # (message_id, request_id) -> ts, dedupes streamed rewrites
    "clients": {},      # client ip -> last request ts, shows who is polling us
    "limits": None,     # latest plan-limit snapshot from the OAuth usage endpoint
    "context": None,    # {tokens, ts} — newest assistant event's context size
    "btc": None,        # {price} from Binance — fetched here so the board needs no TLS
    "weather": None,    # {tempC, code} for Bangkok from Open-Meteo, same reason
    "activity_event": threading.Event(),
    "lock": threading.Lock(),
}


def is_client_active():
    now = datetime.now(timezone.utc)
    with STATE["lock"]:
        # Active if a client requested within the last 3 minutes
        for ip, ts in STATE["clients"].items():
            if (now - ts).total_seconds() <= 180:
                return True
    return False

LIMITS_INTERVAL_SEC = 120
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
BTC_URL = "https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT"
WEATHER_URL = ("https://api.open-meteo.com/v1/forecast?latitude=13.7563"
               "&longitude=100.5018&current=temperature_2m,weather_code"
               "&timezone=Asia%2FBangkok")
BTC_INTERVAL_SEC = 10
WEATHER_INTERVAL_SEC = 600

# The CYD polls every ~20s nonstop; that's frequent enough that macOS never
# accumulates a long idle gap and just never commits to real system sleep.
# Harmless on AC power, but if this Mac is ever left running unattended on
# battery it silently drains to empty overnight instead of sleeping. Pausing
# (fully closing) the listening socket below this threshold lets idle sleep
# proceed; the board already has a graceful OFFLINE/cat-mode fallback for
# exactly this "server unreachable" case.
BATTERY_LOW_PCT = 50
BATTERY_CHECK_INTERVAL_SEC = 60


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
    return f"{dt.strftime('%b')} {dt.day} at {t}" if with_date else t


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


def limits_loop():
    was_empty = False
    last_fetch = 0.0
    first_run = True
    while True:
        if not first_run and not is_client_active():
            with STATE["lock"]:
                STATE["activity_event"].clear()
            STATE["activity_event"].wait(timeout=60)
            continue
        first_run = False

        now = time.time()
        sleep_sec = 5
        if now - last_fetch >= LIMITS_INTERVAL_SEC:
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
                    last_fetch = now - LIMITS_INTERVAL_SEC + sleep_sec
            else:
                sleep_sec = LIMITS_INTERVAL_SEC
        else:
            sleep_sec = max(1, int(LIMITS_INTERVAL_SEC - (now - last_fetch)))

        time.sleep(sleep_sec)


def fetch_btc():
    req = urllib.request.Request(BTC_URL, headers={"User-Agent": "cydusage"})
    with urllib.request.urlopen(req, timeout=8) as resp:
        doc = json.loads(resp.read())
    price = float(doc.get("price") or 0)
    return {"price": price} if price > 0 else None


def fetch_weather():
    # Load API key from secrets.local.json in the repository root
    key = None
    secrets_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "secrets.local.json")
    if os.path.exists(secrets_path):
        try:
            with open(secrets_path, "r") as f:
                secrets = json.load(f)
                key = secrets.get("WEATHER_API_KEY")
        except Exception as e:
            print(f"Error reading secrets: {e}", file=sys.stderr)

    # Fallback to Open-Meteo if no WeatherAPI key is provided
    if not key:
        try:
            with urllib.request.urlopen(WEATHER_URL, timeout=8) as resp:
                doc = json.loads(resp.read())
            cur = doc.get("current") or {}
            if cur.get("temperature_2m") is None:
                return None
            return {"tempC": cur["temperature_2m"], "code": cur.get("weather_code", -1)}
        except Exception as e:
            print(f"Open-Meteo fallback failed: {e}", file=sys.stderr)
            return None

    # Fetch from WeatherAPI
    url = f"http://api.weatherapi.com/v1/current.json?key={key}&q=Bangkok"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "cydusage"})
        with urllib.request.urlopen(req, timeout=8) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        
        cur = data.get("current", {})
        temp_c = cur.get("temp_c")
        if temp_c is None:
            return None
            
        cond = cur.get("condition", {})
        wa_code = cond.get("code", 1000)
        
        # Map WeatherAPI condition codes to WMO weather codes expected by the CYD board
        # Default WMO code 3 (cloudy)
        wmo_code = 3
        if wa_code == 1000:
            wmo_code = 0  # clear
        elif wa_code == 1003:
            wmo_code = 1  # mainly clear / partly cloudy
        elif wa_code in (1006, 1009, 1030, 1135, 1147):
            wmo_code = 3  # cloudy / overcast / fog
        elif wa_code in (1063, 1150, 1153, 1180, 1183, 1186, 1189, 1192, 1195, 1198, 1201, 1240, 1243, 1246):
            wmo_code = 61  # rain
        elif wa_code in (1066, 1069, 1072, 1114, 1117, 1204, 1207, 1210, 1213, 1216, 1219, 1222, 1225, 1237, 1249, 1252, 1255, 1258, 1261, 1264):
            wmo_code = 71  # snow
        elif wa_code in (1087, 1273, 1276, 1279, 1282):
            wmo_code = 95  # thunderstorm
            
        return {"tempC": temp_c, "code": wmo_code}
    except Exception as e:
        print(f"WeatherAPI fetch failed: {e}", file=sys.stderr)
        return None


def market_loop():
    # BTC on a ~10s cadence, weather every 10 min. Each keeps its last good value
    # on failure so a transient outage doesn't blank the board's tiles.
    last_weather = 0.0
    first_run = True
    while True:
        if not first_run and not is_client_active():
            with STATE["lock"]:
                STATE["activity_event"].clear()
            STATE["activity_event"].wait(timeout=60)
            continue
        first_run = False
        try:
            btc = fetch_btc()
            if btc:
                STATE["btc"] = btc
        except Exception as e:
            print(f"market_loop btc: {type(e).__name__}: {e}", file=sys.stderr)
        now = time.time()
        if now - last_weather >= WEATHER_INTERVAL_SEC:
            last_weather = now
            try:
                weather = fetch_weather()
                if weather:
                    STATE["weather"] = weather
            except Exception as e:
                print(f"market_loop weather: {type(e).__name__}: {e}", file=sys.stderr)
        time.sleep(BTC_INTERVAL_SEC)


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


def scan_once():
    new_events = []
    newest_ctx = None  # {tokens, ts} of the newest event in this batch
    for path in glob.glob(LOG_GLOB):
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
            # The same assistant message is often written multiple
            # times while streaming, with identical usage — count once.
            msg_id = message.get("id")
            if msg_id:
                key = (msg_id, entry.get("requestId"))
                if key in STATE["seen"]:
                    continue
                STATE["seen"][key] = ts
            new_events.append({
                "ts": ts,
                "project": project_name(entry.get("cwd")),
                "model": model_family(message.get("model")),
                "tokens": total_tokens(usage),
                "cost": estimate_cost(usage, message.get("model")),
            })
            # Context window of the latest session = the newest assistant
            # event's prompt size (input + both cache tiers, no output).
            ctx_tokens = (
                (usage.get("input_tokens") or 0)
                + (usage.get("cache_read_input_tokens") or 0)
                + (usage.get("cache_creation_input_tokens") or 0))
            if newest_ctx is None or ts > newest_ctx["ts"]:
                newest_ctx = {"tokens": ctx_tokens, "ts": ts}
        STATE["offsets"][path] = offset + len(consumable)

    if new_events:
        with STATE["lock"]:
            STATE["events"].extend(new_events)
            cutoff = datetime.now(timezone.utc) - RETENTION
            STATE["events"] = [e for e in STATE["events"] if e["ts"] >= cutoff]
            STATE["seen"] = {k: ts for k, ts in STATE["seen"].items() if ts >= cutoff}
            prev = STATE["context"]
            if newest_ctx and (prev is None or newest_ctx["ts"] > prev["ts"]):
                STATE["context"] = newest_ctx


def scan_loop():
    first_run = True
    while True:
        if not first_run and not is_client_active():
            with STATE["lock"]:
                STATE["activity_event"].clear()
            STATE["activity_event"].wait(timeout=60)
            continue
        first_run = False
        try:
            scan_once()
        except Exception as e:
            # never let a transient error (e.g. during sleep/wake) kill the thread
            print(f"scan_loop: {type(e).__name__}: {e}", file=sys.stderr)
        time.sleep(SCAN_INTERVAL_SEC)


def build_report():
    now = datetime.now(timezone.utc)
    local_today = datetime.now().astimezone().date()

    with STATE["lock"]:
        events = list(STATE["events"])
        ctx = STATE["context"]

    today_tokens = today_cost = 0
    last5h_tokens = last5h_cost = 0
    week_tokens = week_cost = 0
    project_totals = {}
    model_totals = {}
    day_totals = {}
    last_ts = None

    five_h_ago = now - timedelta(hours=5)
    seven_d_ago = now - timedelta(days=7)

    for e in events:
        ts = e["ts"]
        if last_ts is None or ts > last_ts:
            last_ts = ts

        local_date = ts.astimezone().date()
        if local_date == local_today:
            today_tokens += e["tokens"]
            today_cost += e["cost"]
            mt = model_totals.setdefault(e.get("model", "other"), {"tokens": 0, "cost": 0.0})
            mt["tokens"] += e["tokens"]
            mt["cost"] += e["cost"]

        if ts >= five_h_ago:
            last5h_tokens += e["tokens"]
            last5h_cost += e["cost"]

        if ts >= seven_d_ago:
            week_tokens += e["tokens"]
            week_cost += e["cost"]
            project_totals[e["project"]] = project_totals.get(e["project"], 0) + e["tokens"]
            day_totals[local_date] = day_totals.get(local_date, 0) + e["tokens"]

    top_projects = sorted(project_totals.items(), key=lambda kv: kv[1], reverse=True)[:5]

    # Today's cost split by model family, biggest spender first. Percent is
    # share of today's estimated cost (not tokens) so cheap-model bulk usage
    # doesn't drown out what actually costs money.
    models = []
    for name, mt in sorted(model_totals.items(), key=lambda kv: kv[1]["cost"], reverse=True)[:4]:
        pct = round(mt["cost"] / today_cost * 100) if today_cost > 0 else 0
        models.append({
            "name": name,
            "tokens": mt["tokens"],
            "cost": round(mt["cost"], 4),
            "percent": pct,
        })

    trend = []
    for i in range(6, -1, -1):
        d = local_today - timedelta(days=i)
        trend.append(day_totals.get(d, 0))

    active_now = last_ts is not None and (now - last_ts).total_seconds() <= ACTIVE_WINDOW_SEC
    last_activity_sec = int((now - last_ts).total_seconds()) if last_ts else None

    with STATE["lock"]:
        client_snapshot = list(STATE["clients"].items())
    clients = [
        {"ip": ip, "last_seen_sec": int((now - ts).total_seconds())}
        for ip, ts in sorted(client_snapshot, key=lambda kv: kv[1], reverse=True)
    ]

    # Countdown to the session reset and snapshot age, computed at request
    # time (not when the limits snapshot was fetched) so they stay accurate
    # between OAuth refreshes. Mutate per-request copies only — the snapshot
    # in STATE["limits"] must stay pristine.
    limits = STATE["limits"]
    if limits:
        limits = dict(limits)
        if limits.get("fetched_at_epoch"):
            limits["age_sec"] = max(0, int(time.time() - limits["fetched_at_epoch"]))
        for key in ("session", "week", "week_model"):
            win = limits.get(key) or {}
            if not win.get("resets_at_epoch"):
                continue
            win = dict(win)
            remaining = int(win["resets_at_epoch"] - time.time())
            if remaining <= 0:
                # The window this snapshot describes has already ended; the
                # OAuth loop may not have a fresh one yet (nulled resets_at
                # until new activity, or a 429 backoff). Serve it as reset
                # rather than a stale percent stuck past its own reset time.
                win["percent"] = 0
                win["resets"] = ""
                remaining = 0
            if key == "session":
                win["resets_in_sec"] = remaining
            limits[key] = win

    return {
        "generated_at": now.isoformat(),
        "today": {"tokens": today_tokens, "cost": round(today_cost, 4)},
        "last5h": {"tokens": last5h_tokens, "cost": round(last5h_cost, 4)},
        "week": {"tokens": week_tokens, "cost": round(week_cost, 4)},
        "active_now": active_now,
        "last_activity_sec": last_activity_sec,
        "projects": [{"name": name, "tokens": tokens} for name, tokens in top_projects],
        "models": models,
        "trend": trend,
        "limits": limits,
        "context": ({"tokens": ctx["tokens"],
                     "percent": round(ctx["tokens"] / CONTEXT_WINDOW_TOKENS * 100)}
                    if ctx else None),
        "btc": STATE["btc"],
        "weather": STATE["weather"],
        "clients": clients,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path != "/api/usage":
            self.send_response(404)
            self.end_headers()
            return
        ip = self.client_address[0]
        with STATE["lock"]:
            STATE["clients"][ip] = datetime.now(timezone.utc)
            STATE["activity_event"].set()
        if ip not in ("127.0.0.1", "::1"):
            # One heartbeat line per board poll; local probes stay quiet.
            print(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {ip} GET /api/usage", flush=True)
        body = json.dumps(build_report()).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)


class Server(ThreadingHTTPServer):
    allow_reuse_address = True  # rebind immediately if launchd restarts us
    daemon_threads = True


def start_http_server():
    server = Server(("0.0.0.0", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Serving usage stats on http://0.0.0.0:{PORT}/api/usage")
    return server


def battery_guard_loop():
    # Owns the HTTP server's lifecycle: fully unbinds the listening socket
    # while on battery below BATTERY_LOW_PCT (so the board sees OFFLINE
    # instead of the Mac's battery hitting 0%), and rebinds once the Mac is
    # back on AC or the battery has recovered.
    server = start_http_server()
    paused = False
    while True:
        time.sleep(BATTERY_CHECK_INTERVAL_SEC)
        status = battery_status()
        if status is None:
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
        except Exception as e:
            log_err(f"battery_guard: {type(e).__name__}: {e}")


def main():
    scan_once()
    threading.Thread(target=scan_loop, daemon=True).start()
    threading.Thread(target=limits_loop, daemon=True).start()
    threading.Thread(target=market_loop, daemon=True).start()
    battery_guard_loop()


if __name__ == "__main__":
    main()
