#!/usr/bin/env python3
"""Serves Claude Code token usage stats parsed from ~/.claude/projects/*/*.jsonl."""

import glob
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG_GLOB = os.path.expanduser("~/.claude/projects/*/*.jsonl")
PORT = 8787
SCAN_INTERVAL_SEC = 15
RETENTION = timedelta(days=8)
ACTIVE_WINDOW_SEC = 180

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
    "btc": None,        # {price} from Binance — fetched here so the board needs no TLS
    "weather": None,    # {tempC, code} for Bangkok from Open-Meteo, same reason
    "lock": threading.Lock(),
}

LIMITS_INTERVAL_SEC = 60
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
    t = dt.strftime("%I:%M%p").lstrip("0").lower()
    return f"{dt.strftime('%b')} {dt.day} at {t}" if with_date else t


def oauth_token():
    # Read the Claude Code OAuth token from the macOS Keychain at request
    # time; it is never written to disk or included in responses.
    out = subprocess.run(
        ["/usr/bin/security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
        capture_output=True, text=True, timeout=10,
    )
    if out.returncode != 0:
        return None
    return json.loads(out.stdout).get("claudeAiOauth", {}).get("accessToken")


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
    session = doc.get("five_hour") or {}
    week = doc.get("seven_day") or {}
    if session.get("utilization") is None or week.get("utilization") is None:
        return None
    now = datetime.now().astimezone()
    week_reset = datetime.fromisoformat(week["resets_at"].replace("Z", "+00:00")).astimezone()
    return {
        "tz": TZ_NAME,
        "session": {
            "percent": round(session["utilization"]),
            "resets": fmt_reset_time(session["resets_at"], with_date=False),
            # Raw epoch so resets_in_sec can be computed fresh per request
            # even when this snapshot is minutes old.
            "resets_at_epoch": datetime.fromisoformat(session["resets_at"].replace("Z", "+00:00")).timestamp(),
        },
        "week": {
            "percent": round(week["utilization"]),
            "resets": fmt_reset_time(week["resets_at"], with_date=week_reset.date() != now.date()),
        },
    }


def limits_loop():
    while True:
        try:
            limits = fetch_limits()
            if limits:
                STATE["limits"] = limits
        except Exception as e:
            # keep the last good snapshot; report shows it as-is
            print(f"limits_loop: {type(e).__name__}: {e}", file=sys.stderr)
        time.sleep(LIMITS_INTERVAL_SEC)


def fetch_btc():
    req = urllib.request.Request(BTC_URL, headers={"User-Agent": "cydusage"})
    with urllib.request.urlopen(req, timeout=8) as resp:
        doc = json.loads(resp.read())
    price = float(doc.get("price") or 0)
    return {"price": price} if price > 0 else None


def fetch_weather():
    with urllib.request.urlopen(WEATHER_URL, timeout=8) as resp:
        doc = json.loads(resp.read())
    cur = doc.get("current") or {}
    if cur.get("temperature_2m") is None:
        return None
    return {"tempC": cur["temperature_2m"], "code": cur.get("weather_code", -1)}


def market_loop():
    # BTC on a ~10s cadence, weather every 10 min. Each keeps its last good value
    # on failure so a transient outage doesn't blank the board's tiles.
    last_weather = 0.0
    while True:
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
    for path in glob.glob(LOG_GLOB):
        try:
            size = os.path.getsize(path)
        except OSError:
            continue
        offset = STATE["offsets"].get(path, 0)
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
        STATE["offsets"][path] = offset + len(consumable)

    if new_events:
        with STATE["lock"]:
            STATE["events"].extend(new_events)

    cutoff = datetime.now(timezone.utc) - RETENTION
    with STATE["lock"]:
        STATE["events"] = [e for e in STATE["events"] if e["ts"] >= cutoff]
    STATE["seen"] = {k: ts for k, ts in STATE["seen"].items() if ts >= cutoff}


def scan_loop():
    while True:
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

    # Countdown to the session reset, computed at request time (not when the
    # limits snapshot was fetched) so it stays accurate between OAuth refreshes.
    limits = STATE["limits"]
    if limits and limits.get("session", {}).get("resets_at_epoch"):
        limits = dict(limits)
        session_limits = dict(limits["session"])
        session_limits["resets_in_sec"] = max(
            0, int(session_limits["resets_at_epoch"] - time.time()))
        limits["session"] = session_limits

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


def main():
    scan_once()
    threading.Thread(target=scan_loop, daemon=True).start()
    threading.Thread(target=limits_loop, daemon=True).start()
    threading.Thread(target=market_loop, daemon=True).start()
    server = Server(("0.0.0.0", PORT), Handler)
    print(f"Serving usage stats on http://0.0.0.0:{PORT}/api/usage")
    server.serve_forever()


if __name__ == "__main__":
    main()
