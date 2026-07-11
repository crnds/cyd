# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Claude Code token-usage dashboard for the ESP32-2432S028R "Cheap Yellow
Display" (CYD) — a 2.8″ 320×240 landscape touch screen. Three moving parts:

```
Mac laptop                                   CYD board (ESP32, on WiFi)
server/usage_server.py  ──HTTP /api/usage──▶ firmware/cyd_dashboard/*.ino
(reads local logs + OAuth usage endpoint)    (polls every 20s, 6 tap-to-cycle pages)

simulator.html = a browser stand-in for the board, fetching the same endpoint
```

The Mac is a normal laptop that sleeps/wakes; the board is set up once and
left on a USB charger. There is no build system on the Mac side (plain
Python stdlib); the firmware builds with the ESP32 Arduino toolchain.

## The one invariant that matters most

`firmware/cyd_dashboard/cyd_dashboard.ino` and `simulator.html` render the
**same UI** and MUST be kept in lockstep. Any change to a page's layout,
colors, formatting, or the OFFLINE screen must be made in *both*, using the
*same coordinates and values*:

- The simulator's `gfx` object emulates the LovyanGFX API on a canvas. Its
  `print()` advances the cursor a fixed **6px per character at text size 1**
  (×size), exactly like the hardware's built-in GFX font, so the simulator's
  line lengths predict what fits on the real 320px-wide screen. Do not
  replace this with natural browser text metrics — that would break the
  fidelity that makes the simulator useful for catching overflow.
- Colors are defined once per side and must match: firmware uses **RGB565**
  hex (e.g. `COL_ACCENT = 0xFB08`), the simulator uses the **RGB888**
  equivalent (`rgb(255,97,66)`). When changing a color, convert and update
  both, keeping the `// 0x....` comment in the simulator.
- The page-drawing functions (`drawHomePage`, `drawProjectsPage` — which now
  also renders the 7-day trend in its lower half, `drawStatusPage`,
  `drawDevicePage`, `drawLongTrendPage`, `drawLimitBlock`, `drawFooter`,
  `drawOfflineScreen`) are near line-for-line ports of each other.
- **Exception — page 6 (cat GIFs) is NOT a pixel-faithful twin.** On the device
  it decodes and plays random GIFs from `/cats/` on the SD card frame-by-frame
  (AnimatedGIF, in the firmware's `gifTick()`), which the canvas simulator can't
  emulate. The simulator's `drawGifPage()` shows the same "CATS" title/layout as
  the firmware's no-cats placeholder (`drawGifPlaceholder`) plus a note. Only
  the placeholder state is kept in parity; the actual playback is firmware-only.

After any UI change, verify the simulator with a headless screenshot (see
Commands) — this is the closest thing to visual regression testing here,
since the firmware can't be run without hardware.

## Commands

**Run / test the Mac server:**
```
/usr/bin/python3 server/usage_server.py        # use /usr/bin/python3 — see note below
curl http://localhost:8787/api/usage           # inspect the JSON contract
```

**Verify the simulator (headless, catches JS errors + previews pages):**
```
node -e 'const{chromium}=require("playwright");(async()=>{const b=await chromium.launch();const p=await b.newPage();p.on("pageerror",e=>console.log("ERR",e.message));await p.goto("file://'"$PWD"'/simulator.html");await p.waitForTimeout(2000);await p.locator(".device").screenshot({path:"/tmp/cyd.png"});await b.close();})()'
```
Playwright is installed at the repo-root level. Clicking `#screen` cycles
pages; `#sim-offline` / `#mock-data` checkboxes exercise those states.

**Compile the firmware (no board needed — verifies it builds):**
```
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/cyd_dashboard
```
Requires: `esp32:esp32` core + `LovyanGFX`, `ArduinoJson`, and `AnimatedGIF`
(the page-6 cat player) libs (ESPmDNS ships with the core). A `config.h` must
exist (copy `config.example.h`); it's gitignored. Build is now ≈ 96% flash
(the GIF decoder pushed it up from ~87%) — tight but it fits; watch the budget
when adding code.

**Flash (needs the board plugged in):**
```
arduino-cli board list                          # find the /dev/cu.usbserial-* port
arduino-cli compile --fqbn esp32:esp32:esp32:UploadSpeed=115200 --upload -p <PORT> firmware/cyd_dashboard
```
**ALWAYS flash at the low 115200 baud** (`UploadSpeed=115200` on the FQBN).
This board's USB-serial link is flaky at the 921600 default and fails
mid-write with "The chip stopped responding" — 115200 is slow (~80s) but
reliable. Do not flash at the default speed.

## Server internals (`usage_server.py`)

- **Stdlib only** (`http.server`, `subprocess`, `urllib`) so it can run under
  launchd. The launchd job (`com.eunite.cydusage.plist`) invokes
  **`/usr/bin/python3`, which is 3.9.6** — keep the code 3.9-compatible
  (notably: `datetime.fromisoformat` can't parse a trailing `Z`, hence the
  `.replace("Z","+00:00")`). Test with `/usr/bin/python3`, not just Homebrew.
- **Three data sources:**
  1. `~/.claude/projects/*/*.jsonl` — token counts, tailed incrementally by
     byte offset. Streamed assistant messages are written multiple times with
     identical usage, so events are **deduped on `(message.id, requestId)`,
     keep-first**. Without this, totals roughly double.
  2. `api.anthropic.com/api/oauth/usage` — the real `/usage` session/week
     percentages + reset times. The OAuth token is read at request time by
     shelling out to `/usr/bin/security` (Keychain); it is never written to
     disk or included in responses. A background thread refreshes this every
     60s and keeps the last good snapshot on failure.
  3. `/etc/localtime` symlink — local timezone name for reset-time display.
  4. **BTC (Binance) + Bangkok weather (Open-Meteo)** — fetched by a background
     `market_loop` (BTC ~10s, weather 10 min), last-good kept on failure. Done
     Mac-side ON PURPOSE: the CYD has no PSRAM, and its 154KB framebuffer leaves
     too little contiguous heap for an mbedTLS handshake, so the board could not
     fetch these HTTPS endpoints itself (TCP 443 connected but the TLS buffer
     alloc failed). Proxying them through this plain-HTTP endpoint keeps all TLS
     off the board — the firmware has no `WiFiClientSecure` at all now.
- **Endpoint `GET /api/usage`** returns one JSON blob. Both clients parse it,
  so it's a contract — changing a field name means editing the firmware and
  simulator too. Current keys: `today`/`last5h`/`week` (`{tokens,cost}`),
  `active_now`, `last_activity_sec`, `projects[{name,tokens}]`,
  `models[{name,tokens,cost,percent}]` (today's cost split by model family,
  top 4 by cost, percent = share of today's estimated cost), `trend[7]`,
  `limits{tz,session{percent,resets,resets_at_epoch,resets_in_sec},week{percent,resets}}`
  (null if the keychain read hasn't succeeded; `resets_in_sec` is computed at
  request time so it stays fresh between OAuth refreshes),
  `btc{price}` / `weather{tempC,code}` (null until the first market fetch),
  `clients[{ip,last_seen_sec}]`, `generated_at`.
- Dollar costs are **estimates** from a hardcoded per-model price table.
- Remote (non-localhost) requests are logged one line each to stdout —
  `tail -f /tmp/cydusage.log` is a live heartbeat of the board's polling.

## Control panel (`server/control_server.py` + `server.html`)

- A second launchd job (`com.eunite.cydcontrol.plist`) serving
  `http://127.0.0.1:8788/` — a status page (`server.html`) plus
  `GET /api/status` and `POST /api/enable|/api/disable`, which shell out to
  `launchctl bootstrap`/`bootout` on the `com.eunite.cydusage` job.
- **Localhost-bound on purpose** (it can execute launchctl) — never bind it
  to `0.0.0.0`.
- It's deliberately a separate process/port so the page stays up while the
  usage server is disabled. `server.html` follows the parent `~/CLAUDE.md`
  static-web conventions and is NOT bound by the firmware-parity rule.
- Both plists live in `server/` and are installed to
  `~/Library/LaunchAgents/`; editing the copies in the repo does nothing
  until re-copied + `launchctl kickstart -k gui/$UID/<label>`.

## Firmware internals (`cyd_dashboard.ino`)

- Landscape `setRotation(1)`. Pin map for the resistive-touch (XPT2046)
  variant is in `pins.h`; capacitive (GT911) units need different pins.
- **Two cores, one lock.** ALL blocking I/O (usage poll, BTC, weather, mDNS,
  WiFi reconnect, SD writes) runs in `networkTask` pinned to core 0; `loop()`
  on core 1 does only touch + render, so a slow/hung fetch can never freeze
  the UI. **Never add a network call to `loop()`** — that reintroduces the
  multi-minute touch freezes this split was built to fix. The two cores share
  `STATE`/`longTrend[]` through `stateMutex` (`lockState`/`unlockState`): the
  fetch helpers hold it only for the brief copy of parsed results into `STATE`
  (never during the network wait), and `render()` holds it while drawing
  (which reads `STATE` String members the task may reassign) and releases
  before `pushSprite`. It is **non-recursive** — draw helpers must not re-lock.
- **No TLS on the board.** The firmware makes only plain-HTTP calls (the
  `/api/usage` poll). BTC + weather used to be fetched here over HTTPS, but this
  no-PSRAM board can't spare a contiguous ~34KB heap block for the mbedTLS
  buffers alongside the 154KB framebuffer, so those handshakes failed and the
  tiles showed "--". They now arrive inside `/api/usage` (the Mac fetches them —
  see the server's `market_loop`); `applyUsageJson` copies `btc`/`weather` into
  `STATE`. Do NOT reintroduce `WiFiClientSecure` here — it will silently fail.
- **Touch is NOT on the display's SPI bus.** On this board the XPT2046 has
  its own pins (SCLK 25, MOSI 32, MISO 39, CS 33, IRQ 36) and is driven as
  software SPI (`spi_host = -1`, `bus_shared = false`). Configuring it as
  shared-bus on VSPI silently reports no touches ever — verified on the
  physical board.
- **Server discovery by name, not IP:** `SERVER_HOST` is normally a `.local`
  Bonjour name, resolved via mDNS (`resolveServer()`), so the board tracks
  the Mac across the IP changes a sleeping laptop causes. The resolved IP is
  cached and re-resolved on any fetch failure.
- **Self-healing for "set up once and leave":** `WiFi.setAutoReconnect(true)`,
  non-blocking reconnect in `networkTask` (core 0), and an `ESP.restart()`
  after ~15 min (`RESTART_AFTER_CYCLES`) of no WiFi.
- **Offline = the cat GIFs + an "OFFLINE" banner**, not a dimmed dashboard.
  `loop()` computes `catMode = (currentPage == GIF_PAGE) || offline`, so cats
  play on *any* page whenever offline, and `gifTick(offline)` overlays
  `drawOfflineBanner()` (textSize 5, top-center) on top of the frame. `STATE.haveData`
  is otherwise sticky-true once any fetch or SD cache load succeeds, so
  `fetchFailCycles`/`OFFLINE_AFTER_CYCLES` (~1 min of consecutive failed polls,
  WiFi down or WiFi up but the Mac unreachable) is what forces it back to
  false — without that, a real outage would just freeze the last-known
  dashboard forever instead of ever showing offline.
- Token fields are `int64_t` (weekly totals exceed the 32-bit `long` range).
- **Page 6 = cat GIF player.** `gifTick()` (called from `loop()` on core 1)
  decodes at most one frame per pass via AnimatedGIF and paces itself with
  `gifNextFrameMs`, so touch stays responsive; when a GIF ends it opens another
  at random from `catFiles[]` (scanned once at boot by `scanCats()`) — endless.
  `render()` early-returns for `GIF_PAGE`; `loop()` skips the footer/progress
  line whenever `catMode` is active (page 6, or offline on any page) so the
  cats own the whole screen. This is the one deliberate break from the
  firmware/simulator parity rule.
- **`sdMutex` — the second lock.** Page 6 is the only code that reads the SD
  card from the render core (core 1); all other SD I/O is on `networkTask`
  (core 0). `sdMutex` (via `lockSD`/`unlockSD`) serializes the HSPI/SD bus
  between the two. Only nesting allowed is `sdMutex` → `stateMutex` (as in
  `appendArchiveRow`/`saveEnvCache` under `fetchUsage`'s SD block); nothing takes
  `sdMutex` while holding `stateMutex`, so `render()` never blocks on the card.
  (The display bus is VSPI and stays single-core, so it needs no such lock.)
- **SD card layout** (all optional; every read/write no-ops when `sdOk` is
  false, and any missing file is treated as "absent", never an error). All SD
  I/O runs on `networkTask` (core 0) — never add SD access to `loop()`:
  - `/config.json` — runtime overrides for the compiled `config.h`
    (`loadRuntimeConfig`): `wifi_ssid`, `wifi_password`, `server_host`,
    `server_port`, `brightness` (0-255), `poll_interval_sec` (clamped 5-3600).
    Lets a set-once board be retuned without reflashing.
  - `/last_usage.json` — last-good `/api/usage` blob; `/last_env.json` — last
    BTC/weather. Both restored at boot so the dashboard shows real (if stale)
    data and the BTC/weather tiles aren't blank while the Mac is unreachable.
  - `/daily_log.csv` — one end-of-day token row, backs the on-screen 30-day
    trend (`longTrend[]`). `/archive.csv` — fine-grained **one row per poll**
    (~20s, unbounded, ~1GB/yr) for off-device analysis; NOT shown on screen.
  - `/diag_log.csv` — black-box event log (`logDiag`): boot + reset reason,
    WiFi down/recovered, self-reboot, hourly heap/uptime, one-shot low-heap.
  - `/splash.bmp` — optional 320×240 24-bit boot splash, shown ~1.5s before the
    WiFi spinner (`drawBmpFromSD`/`showBootSplash`). A self-hosted asset loaded
    from SD rather than baked into the near-full flash; absent → no splash.
  - `/cats/*.gif` — the page-6 cat GIF library (see the GIF-player note above).
    Prepared board-side by `prepare_cat_gifs.py` (downloads from Cataas, resizes
    to ≤320×240, thins frames, optimizes with gifsicle); absent → page 6 shows
    the "CATS" placeholder instead of playing.

## Conventions

`simulator.html` follows the static-web conventions in the parent
`~/CLAUDE.md` (a `:root` design-token set, kebab-case classes, no build
step). The dashboard UI colors there are dictated by the firmware parity
requirement above, not by those tokens.
