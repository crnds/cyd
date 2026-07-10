# Full codebase audit + live diagnosis

**Date:** 2026-07-10  
**Repo:** `~/cyd` — Claude Code token-usage dashboard for ESP32-2432S028R (CYD)  
**Auditor:** Grok (xAI)

---

## Live system status (at audit time): **healthy**

| Check | Result |
|---|---|
| `com.eunite.cydusage` launchd | running, pid active |
| `com.eunite.cydcontrol` launchd | running |
| `GET /api/usage` | 200, ~18 ms, ~917 B JSON |
| Plan limits (keychain → OAuth) | OK — session **50%**, week **11%** |
| Board heartbeat | `192.168.1.175` polling ~every **20s** |
| `/tmp/cydusage.err` | empty |
| Hostname | `eUnite-MBA-M3-DN-2.local` matches `config.h` + `scutil` |
| Simulator (Playwright) | connected, **no JS errors**, all 5 pages render |

Nothing was “totally broken” at audit time. Findings are **real bugs**, **historical flakiness**, **docs drift**, and a few **correctness/UX traps**.

---

## Architecture (context)

```
Mac laptop                                   CYD board (ESP32, on WiFi)
server/usage_server.py  ──HTTP /api/usage──▶ firmware/cyd_dashboard/*.ino
(reads local logs + OAuth usage endpoint)    (polls every 20s, 5 tap-to-cycle pages)

simulator.html = browser stand-in for the board
server.html + control_server.py = localhost control panel (8788)
```

---

## What’s working

1. End-to-end path: Mac server → board poll → JSON contract  
2. Dedup on `(message.id, requestId)` — ~**8.8k unique** vs ~**8.8k stream dupes** skipped (would roughly double without it)  
3. mDNS hostname config is correct; board is on LAN  
4. Firmware ↔ simulator page structure is largely in lockstep (5 pages, device stats, BTC on home)  
5. Control panel diagnostics match reality  
6. Python sources `py_compile` clean under `/usr/bin/python3`  
7. Most RGB565 → RGB888 color mappings match within 1 LSB  

---

## Not a bug (but looks wrong)

### `last5h` ≫ `today` (17.8× at audit time)

| Window | Tokens |
|---|---|
| Today (local **2026-07-10**) | ~1.2M |
| Last 5 hours | ~21.5M |

Local time was **Asia/Bangkok, just after midnight**. Almost all of the last 5h was still **July 9 evening**. Server math matched a full recompute from the jsonl logs.

**Diagnosis:** expected right after local midnight — not corruption.

Hourly breakdown (local, last 8h, deduped):

```
2026-07-09 18:00     6,827,124
2026-07-09 22:00     5,503,985
2026-07-09 23:00    14,795,121
2026-07-10 00:00     1,206,230
```

---

## Real bugs (ordered by severity)

### 1. JSONL tail can **skip the last incomplete line forever** — medium/high

In `scan_once()`, after reading a file the offset is always set to `f.tell()`:

```python
for line in f:
    ...
    try:
        entry = json.loads(line)
    except ValueError:
        continue   # incomplete/corrupt line skipped
STATE["offsets"][path] = f.tell()  # advanced past the bad partial line
```

If Claude is mid-write when the scanner hits EOF, the partial line fails `json.loads`, the offset jumps past it, and when the line is completed later **that event is never counted**.

**Fix:** only advance offset to the start of the last incomplete line (or only past lines that end with `\n`).

---

### 2. Stale **limits** on firmware + simulator when API returns `limits: null` — medium

Firmware (`fetchUsage`) and simulator (`applyData`) only update limits **when present**:

```cpp
if (!doc["limits"].isNull()) {
  STATE.sessionPercent = ...
}
// no else → old % stays forever
```

If the usage server restarts and keychain/OAuth fails (or returns null), the board can show **yesterday’s session %** until reboot.

**Fix:** explicit else branch resetting `sessionPercent` / `weekPercent` / reset strings to `-1` / empty.

---

### 3. OAuth timestamp `Z` suffix will break `fetch_limits` on system Python 3.9 — medium (latent)

Verified on `/usr/bin/python3` (3.9):

| String | `fromisoformat` |
|---|---|
| `...+00:00` | OK |
| `...Z` | **ValueError** |

Log timestamps already use `.replace("Z", "+00:00")`, but `fmt_reset_time` / `fetch_limits` do **not**. At audit time limits worked (API likely sending offset form). If Anthropic ever sends `Z`, `limits_loop` swallows the exception and limits stick or stay null with **no log line**.

**Fix:** same `replace("Z", "+00:00")` on all OAuth timestamps; log failures to stderr.

---

### 4. Silent failure swallowing — medium (ops)

Both background loops in `usage_server.py`:

```python
except Exception:
    pass
```

`/tmp/cydusage.err` was empty even when OAuth could 429 (reproduced with a second client). Hard to diagnose “why are limits `--`?” without instrumenting.

**Fix:** log exception type/message (rate-limited) to stderr.

---

### 5. Intermittent board offline / long poll gaps — medium (runtime, not pure code)

From `/tmp/cydusage.log` (only **successful** remote polls are logged):

- Many **~15 min** gaps earlier (Mac sleep / lid closed — expected)  
- Many **80–100s** gaps even while awake (failed polls in between; board would show **OFFLINE** for each failure)  
- Steady **20s** again after ~23:59  

Firmware treats **any** failed fetch as full OFFLINE (no “stale last-good” UI):

```cpp
connected = fetchUsage();
if (!connected) drawOfflineScreen();
```

Also, every HTTP non-200 **clears mDNS cache**, so one blip forces re-resolve next cycle — can cascade on flaky WiFi/mDNS.

Contributing factors likely:

- Mac sleep (15‑min class gaps)  
- `fetchBtcPrice()` TLS to Binance every 20s on ESP32 (heap/time pressure)  
- Aggressive `serverIpResolved = false` on any failure  

**Diagnosis path on the board:** serial log; watch whether OFFLINE coincides with BTC fetch or only Mac sleep.

---

### 6. Cost model missing **`fable`** (+ fragile `model_family`) — low/medium

Live log model IDs at audit time:

| Model ID | Family extracted | Count |
|---|---|---|
| `claude-opus-4-8` | opus | 3747 |
| `claude-sonnet-4-6` / `claude-sonnet-5` | sonnet | 3959 |
| **`claude-fable-5`** | **fable** | **1143** |
| `<synthetic>` | other | 2 |

`PRICING` only knows opus/sonnet/haiku → **fable is billed at sonnet rates** via `DEFAULT_PRICING`. Dollar totals for weeks with fable use are biased.

`model_family()` does `split("-")[0]` after stripping `claude-`. That works for current IDs (`opus`/`sonnet`/`fable`) but would mis-label older `claude-3-5-sonnet-…` as `"3"`.

---

### 7. Dead fields / dead UI code after redesign — low (confusing)

Still **parsed into STATE** but **never drawn**:

- `todayTokens` / `todayCost` / `last5h*` / `weekCost`  
- `activeNow` / `lastActivitySec`  
- `haveData`  
- helpers `drawCard`, `fmtAgo` (firmware + simulator)

API still exposes them; control panel doesn’t show activity either. Easy to think “tokens/active are broken” when the UI simply stopped showing them.

---

### 8. Docs out of date — low

| README / CLAUDE claim | Code reality |
|---|---|
| 4 pages; page 3 = ACTIVE/IDLE | **5 pages**; page 3 = Limits + Models; page 5 = Device Stats |
| Home shows today’s/week token totals | Home = session/week bars + **BTC only** |
| `page 1/4` in simulator HTML | `PAGE_COUNT = 5` (label fixed after first render) |

---

### 9. Simulator color parity nits — low

| Token | RGB565 → true RGB888 | Simulator |
|---|---|---|
| `COL_TRACK` `0x5ACB` | ~(90,89,90) | `rgb(88,88,88)` **mismatch** |
| `COL_BLUE` `0x3C1E` | ~(57,129,246) | `rgb(59,130,246)` **mismatch** |

Other colors are within 1 LSB.

---

### 10. Thread-safety footgun on `STATE["clients"]` — low

`Handler` writes `clients[ip]` without the lock; `build_report` does `sorted(STATE["clients"].items())`. Under concurrent polls this can rare-crash with “dictionary changed size during iteration”. CPython GIL makes it uncommon, not impossible.

---

### 11. OAuth can 429 — low (observed)

Hitting the usage endpoint manually while `limits_loop` runs every 60s returned **HTTP 429**. Design keeps last good snapshot (good), but first-boot under rate limit means `limits: null` until a successful refresh.

---

## Historical flakiness pattern (board)

Successful poll gaps from log (not continuous 20s all day):

```
~15 min clusters  → Mac sleep / unreachable
~80–100 s bursts  → failed polls (board OFFLINE flashes)
steady 20s        → healthy awake state
```

That matches the “laptop sleeps; board shows OFFLINE” design, plus some extra flakiness beyond pure sleep.

Example large gaps from `/tmp/cydusage.log` during audit:

```
gap  920–940 s  (~15 min) — repeated through evening
gap   80–100 s  — intermittent failed polls
gap     ~20 s   — normal after recovery
```

---

## Live data snapshot (audit time)

```
generated_at: ~2026-07-09T17:05 UTC (local 2026-07-10 00:05 +07)
today:   ~1.21M tokens, ~$0.76
last5h:  ~21.5M tokens, ~$10.23
week:    ~288M tokens, ~$170
active_now: true
limits:  session 50% resets 2:50am; week 11% resets Jul 16 at 5:00am
clients: 127.0.0.1, 192.168.1.175
models today: sonnet 100%
top projects (7d): org-chart, ampypay-eunite, cyd, autoclaude, server
```

Dedup stats over full corpus:

```
unique usage events: 8851
stream dupes skipped: 8768
no_message_id: 0
types with usage: assistant only
```

---

## Simulator headless smoke (Playwright)

```
pageStatus = page 1/5 (after cycles)
conn = connected
errors = []
screenshots: /tmp/cyd-audit-p0.png … p5.png
```

Pages confirmed:

0. Home — session/week bars + BTC  
1. Top Projects (7d)  
2. Limits + Models (1d)  
3. 7-Day Trend  
4. Device Stats (CPU / flash / static RAM)  

---

## Quick diagnosis checklist (re-run anytime)

```bash
# 1. Jobs
launchctl print gui/$UID/com.eunite.cydusage | head -40
launchctl print gui/$UID/com.eunite.cydcontrol | head -20

# 2. API + limits
curl -s http://127.0.0.1:8787/api/usage | python3 -m json.tool | head -80

# 3. Control panel view of same
curl -s http://127.0.0.1:8788/api/status | python3 -m json.tool | head -60

# 4. Board heartbeat
tail -20 /tmp/cydusage.log
# expect 192.168.1.175 every ~20s when Mac is awake

# 5. Errors (should be empty unless you add logging)
tail -20 /tmp/cydusage.err

# 6. Simulator
open ~/cyd/simulator.html
# or headless Playwright screenshot from CLAUDE.md
```

---

## Recommended fix order

1. **JSONL incomplete-line offset** (silent undercount)  
2. **Clear limits when null** (firmware + simulator)  
3. **Z-safe OAuth timestamps + log exceptions** (server)  
4. **Add `fable` pricing** (or map unknown families explicitly)  
5. Optional: keep last-good dashboard for N failed polls instead of instant OFFLINE; don’t invalidate mDNS on every failure  
6. Sync README page table with the actual 5-page UI  
7. Fix `COL_TRACK` / `COL_BLUE` simulator parity  
8. Lock `STATE["clients"]` reads/writes  

---

## Bottom line

**At audit time the stack was up:** server, control panel, limits, board polling, simulator all green.

The main *correctness* bugs are:

- log tail offset on partial lines  
- stale limits when OAuth fails  
- latent Python 3.9 `Z` parsing  

The main *felt* issues are:

- OFFLINE flashes on any failed poll  
- historical ~15 min / ~90 s poll gaps  
- docs still describing the old 4-page ACTIVE/IDLE UI  
- token/activity data still collected but no longer shown  

---

## Files reviewed

```
cyd/
├── README.md
├── CLAUDE.md
├── simulator.html
├── server.html
├── server/
│   ├── usage_server.py
│   ├── control_server.py
│   ├── com.eunite.cydusage.plist
│   └── com.eunite.cydcontrol.plist
└── firmware/cyd_dashboard/
    ├── cyd_dashboard.ino
    ├── pins.h
    ├── config.example.h
    └── config.h (local only; not committed)
```
