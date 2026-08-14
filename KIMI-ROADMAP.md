# CYD Dashboard — Verified Status & Roadmap (Kimi audit, 2026-08-10)

Ground-up verification of the running system plus the development roadmap,
grouped by **new features → UI/UX → performance**. Every "already exists"
claim below was checked against the live server, a headless simulator render,
a clean firmware compile, and the source — not assumed from docs.

## Verified project status (2026-08-10)

| Check | Result |
|---|---|
| Usage server | Live on `:8787`, full `/api/usage` contract present (`today`/`week` tokens+cost, `projects`, `trend[7]`, `limits` incl. credits, `context`, `btc`, `weather`, `aqi`, `power`, `clients`). Board (`172.17.3.131`) and simulator (`127.0.0.1`) both polling. |
| Simulator | Headless Playwright render of all-good status page: session/week bars, analog clock, AQI badge, weather strip, BTC — no JS errors. |
| Firmware | `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app` — clean build, 1,333,831 B = **42%** of the 3 MB app partition, 17% dynamic memory. Headroom for everything below. |
| Page count | **7 pages (0–6)**: status, projects+trend, home, device, limits, cats, mixed. |

Verified premises the roadmap relies on:

- Server hardcodes `CONTEXT_WINDOW_TOKENS = 200000` (`server/usage_server.py:29`) — ctx% is wrong for 1M-context models. **Confirmed.**
- 5-min buckets keyed by `(project, model)` with 8-day retention exist server-side (`usage_server.py:51`) — pace, per-model, and hourly-heat features need **no new data collection**. **Confirmed.**
- Firmware parses `today.tokens`/`today.cost` only into the SD archive row (`net.cpp:397-413`) — never displayed. Free feature. **Confirmed.**
- Settings are table-driven (`SettingDef SETTINGS[]` in `settings.cpp`) — adding a setting is one row + labels + get/apply pair. **Confirmed.**
- In-flight uncommitted work at audit time: mDNS fail-streak resilience (`net.cpp`), analog-clock second-hand fix + weather-overlay AQI badge (`pages.cpp`), simulator/CLAUDE.md parity. Compile verified with these included.

Hard constraints every item must respect (from `CLAUDE.md`):

- Firmware ↔ simulator pixel parity for any page/layout/color change.
- No network or SD I/O on the render core (`loop()`); no TLS on the board.
- Server stays stdlib-only, Python 3.9-compatible (launchd runs `/usr/bin/python3`).
- Page count is a UX budget: 7 today — add nothing without merging/removing.

Impact/effort tags: **I** = user-visible impact (High/Med/Low), **E** = effort (S < 1 day, M = days, L = week+).

## Track 1 — New features (make the dashboard more useful)

1. **Today's tokens & cost on Home** — I:High E:S
   `today.tokens`/`today.cost` are already in the payload. Add to `STATE`
   (`state.h`), copy in `applyUsageJson` (`net.cpp`), render "TODAY 1.2M ·
   $3.40" on the home page (`pages.cpp`), mirror in `simulator.html`.
2. **Burn-rate / pace indicator** — I:High E:S
   Session % is static; pace is actionable. Server computes tokens/hour over
   the current 5h window from existing buckets at request time (same pattern
   as `resets_in_sec`), adds `limits.session.pace{tokens_per_hr, pace_pct,
   eta_sec}`. Board: green/amber/red pace dot + "limit in 3h12m" on the
   status page. Simulator parity.
3. **Threshold alerts** — I:High E:S
   Session or week % crossing 80/95 → invert-flash the footer once/minute +
   backlight pulse (reuse `checkHourlyFlash` + `applyEffectiveBrightness`).
   One new table-driven Setting (OFF/ON) as kill-switch.
4. **Per-model breakdown row** — I:Med E:S/M
   Buckets are already keyed by `(project, model)`. Server adds a top-3
   `models[]` array (7d); board shows "Opus 62% · Sonnet 38%" on the
   projects page.
5. **Cache-efficiency metric** — I:Med E:M
   jsonl events carry `cache_read_input_tokens`; server keeps a per-window
   hit-rate ratio, board shows "cache hit 7d: 91%". High cache-read ratio =
   efficient prompting. Needs buckets to retain the cache-read split (today
   it's folded into cost only).
6. **Hourly activity heat strip** — I:Med E:M
   24 ticks of today's tokens/hour from the 5-min buckets — "when do I
   actually use Claude". Server adds an hourly array; fits the projects page
   bottom.
7. **Context-window history sparkline** — I:Med E:M
   `context.tokens` arrives every poll; keep a 60-sample ring buffer on the
   board, draw a 1-px sparkline behind the context row on the limits page.
   Compaction events show as visible drops — a signal no other tool surfaces.
8. **Mac system stats in Device Stats** — I:Low/Med E:M
   Server already runs `pmset`; add Mac CPU%, battery%, uptime to the payload
   and a second column on the Device Stats page. Board becomes a mini
   Mac-monitor.

## Track 2 — UI/UX

9. **Auto-rotate carousel** — I:High E:S
    Setting: cycle pages every N seconds when untouched; any touch pauses.
    A desk display you never have to tap is the point of the form factor.
    Trivial timer in `loop()`; one SettingDef row (OFF/15s/30s/60s).
10. **Long-press gestures** — I:Med E:S
    Long-press right → page 0; long-press left → cats. Touch handler already
    tracks down/up timestamps; ~600ms threshold, must not fire on drags.
11. **Swipe vs tap disambiguation** — I:Low E:S
    Extend the settings-list drag-delta logic to page cycling so an
    accidental brush doesn't flip pages.
12. **Configurable ambient card** — I:Med E:M
    BTC symbol is hardcoded. One server-side JSON key picks the Binance
    symbol (ETH, SOL, …); the board just renders the label+price strings it
    already receives.
13. **Boot page "Last viewed"** — I:Low E:S
    `boot_page` flash config exists; persist last-viewed page on change
    (debounced via `pendingConfigSave`) and offer "Last viewed" in the leaf.
14. **Sunrise/sunset-aware night mode** — I:Low E:M (optional)
    Night mode is fixed 23:00–07:00; Open-Meteo already returns daily
    sunrise/sunset on the weather path. Bangkok-centric convenience.

## Track 3 — Performance & robustness

15. **Real context-window size** (correctness, listed here because it fixes
    silently-wrong data) — I:Med E:S
    Parse the actual model limit from the OAuth usage response (or a
    per-model table); fall back to 200K. One server-side change.
16. **Back out of the 8-bit framebuffer** — I:Med E:M
    Boot log shows the 16-bit sprite (153.6 KB) alloc fails → 8-bit palette
    conversion on every push. Options: shrink heap pressure before
    `createSprite` (defer SD/config load order), or a 16-bit half-height
    sprite (320×120, 76.8 KB) with two-pass present. Better colors, cheaper
    pushes; needs care with the GIF player.
17. **GIF decode off the render critical path** — I:Med E:M
    `gifTick` decodes a full frame synchronously in `loop()`. Cap per-tick
    decode time or pre-decode the next frame during idle ticks — keeps touch
    latency flat on pages 5–6.
18. **Payload delta encoding** — I:Low E:M
    `/api/usage` is fully re-parsed every 20s. An etag/version field letting
    the board skip unchanged sections (weather, projects) cuts parse time
    and `stateMutex` hold time.
19. **Real CPU metric** — I:Low E:S
    `cpuPercentAvg` is a loop duty-cycle proxy. Enable FreeRTOS runtime
    stats for true idle %, or at least relabel "CPU" → "LOAD" so it doesn't
    overclaim.
20. **Web-based config after join** — I:Med E:M
    The board already has `WebServer` for the AP portal; keep it running
    (mDNS-advertised) after join so settings can be edited from a browser.
    Gate behind #16 — WebServer + framebuffer may not coexist on the 8-bit
    fallback heap.
21. **OTA firmware updates** — I:Med E:L
    HTTP-pull from the Mac (server serves `/firmware.bin` + version check)
    removes the USB cable from the iteration loop. Needs repartitioning —
    weigh against the `huge_app` simplicity that currently works well.

## Bigger bets (only after the above lands)

22. **Focus/Pomodoro page** — I:Med E:M — clock + countdown rendering
    already exists; mostly reuse. Fits the desk-companion identity.
23. **Local notification relay** — I:Med E:L — Claude Code hooks POST to the
    server → board banner ("Claude needs input"). Highest wow-per-effort here.
24. **Multi-board / multi-machine** — I:Low/Med E:L — only if a second board
    actually exists.
25. **Historical graphs on SD** — I:Low E:L — a 30-day page was deliberately
    removed before (`af2e232`); revisit only with a clear reason.

## Non-goals (keep the project small)

- No cloud accounts, no phone app, no TLS on the board.
- No new pages without removing or merging one — budget is **7 pages**.
- No visual-identity overhaul; the `design.md` system stands.

## Suggested order

Track 1 items 1–3 (all S, server data already exists, immediate payoff) →
15 (correctness fix, server-only) → 9 + 10 (UX) → pick from 4–8 by what you
actually glance at → 16/17 (perf debt that unlocks heap headroom for 20) →
bigger bets only on demand.

## Verification checklist for any item

1. Headless simulator screenshot (Playwright one-liner in `CLAUDE.md`) —
   catches JS errors + layout overflow.
2. `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app
   firmware/cyd_dashboard` — re-check the byte count after large additions.
3. If the JSON contract changed: update the key list in `CLAUDE.md` and keep
   firmware + simulator parsers in lockstep.
